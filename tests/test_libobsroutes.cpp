// libobs Supervisor routes (playout/supervisor/libobsroutes.cpp, docs/libobs.md): the lane registrar driven the way
// SupervisorCore drives it (laneext::installAll on a Context) against a real RestApiServer on an ephemeral loopback port and
// a temp ScheduleStore. Covers: the module is registered as "libobs"; every route is mounted and travels with copyRoutes;
// the catalog shape; spec validation (accepts the spike's example, refuses unknown items / filters / transitions, duplicate
// names, a bad program, a bad fit); the scene store over the wire (PUT list, POST upsert, DELETE, GET) with the WS broadcast;
// the command builders (show / switch / hide) produce the engine's layer command; the per-channel routes answer 404 for an
// unknown saved scene, 422 for a bad spec and 503 when there is no loopback port (restPort 0: no engine is ever contacted).
// Nothing here touches the live stack (port 0, temp dirs, no Supervisor process, no libobs).
#include <QtTest>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTemporaryDir>
#include "../playout/schedule/schedulestore.h"
#include "../playout/supervisor/laneext.h"
#include "../playout/supervisor/restapiserver.h"

namespace libobsroutes {
const QJsonObject& catalog();
QString validateSpec(const QJsonObject& in, QJsonObject* normalized);
QString validateSaved(const QJsonObject& in, QJsonObject* out);
QString validateList(const QJsonArray& in, QJsonArray* out);
QJsonObject showCommand(const QJsonObject& spec, int layer, const QJsonObject& opts);
QJsonObject switchCommand(int layer, const QString& to, const QJsonObject& transition);
QJsonObject hideCommand(int layer, bool remove, double fadeSec);
QJsonObject layerFromTelemetry(const QJsonObject& state, int preferLayer);
}

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }

struct HttpReply { int status = 0; QByteArray body; };

/// One HTTP/1.1 request on a fresh loopback socket; waits at most 10 s for the full reply.
HttpReply http(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = QByteArray()) {
    HttpReply r;
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    if (!s.waitForConnected(3000)) return r;
    QByteArray req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!body.isEmpty()) req += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    req += "\r\n" + body;
    s.write(req);
    QByteArray all;
    QElapsedTimer t; t.start();
    while (t.elapsed() < 10000) {
        if (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0) break;
        // The server lives on this same thread: pump its event loop so it can accept, route and answer.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        s.waitForReadyRead(50);
        all += s.readAll();
        const int hdrEnd = all.indexOf("\r\n\r\n");
        if (hdrEnd < 0) continue;
        const QByteArray hdr = all.left(hdrEnd).toLower();
        const int cl = hdr.indexOf("content-length:");
        if (cl >= 0) {
            const int len = hdr.mid(cl + 15, hdr.indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
            if (all.size() - hdrEnd - 4 >= len) break;
        }
    }
    const int hdrEnd = all.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return r;
    r.body = all.mid(hdrEnd + 4);
    r.status = all.mid(9, 3).toInt();
    return r;
}
QJsonObject json(const QByteArray& b) { return QJsonDocument::fromJson(b).object(); }
QByteArray bytes(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

/// The spike's example spec (docs/libobs-spike.md §3.1), minus the file paths.
QJsonObject exampleSpec() {
    return json(R"({ "type": "libobs", "audio": true,
      "scenes": [
        { "name": "A", "items": [
            { "id": "color_source_v3", "name": "bg", "settings": { "color": "#2040c0" } },
            { "id": "text_ft2_source_v2", "name": "cap", "settings": { "text": "LIVE", "font": { "face": "Arial", "size": 72 } }, "fit": "inner",
              "filters": [ { "id": "color_filter_v2", "settings": { "opacity": 0.8 } } ] } ] },
        { "name": "B", "items": [ { "id": "random", "name": "noise" } ] } ],
      "program": "A",
      "transition": { "id": "fade_transition", "durationMs": 500 } })");
}

/// A Supervisor stand-in: temp store, REST on port 0, the laneext context SupervisorCore builds (restPort 0 = no loopback).
struct Fixture {
    QTemporaryDir dir;
    sched::ScheduleStore store;
    std::unique_ptr<RestApiServer> rest;
    QObject parent;
    QList<QJsonObject> broadcasts;
    laneext::Context ctx;
    bool init() {
        if (!dir.isValid() || !store.open(dir.filePath(S("sup.db")))) return false;
        rest = std::make_unique<RestApiServer>(nullptr);
        if (!rest->start(0)) return false;
        ctx.parent = &parent; ctx.rest = rest.get(); ctx.store = &store; ctx.dataDir = dir.path(); ctx.restPort = 0;
        ctx.restToken = []() { return QString(); };
        ctx.broadcast = [this](const QString& type, const QJsonObject& p) { QJsonObject o = p; o["type"] = type; broadcasts << o; };
        laneext::installAll(ctx);
        return true;
    }
    quint16 port() const { return rest->port(); }
};

} // namespace

class TestLibObsRoutes : public QObject {
    Q_OBJECT
private slots:
    void registered() { QVERIFY(laneext::registered().contains(S("libobs"))); }

    void routesMounted() {
        Fixture f; QVERIFY(f.init());
        const QStringList keys = f.rest->routeKeys();
        for (const char* k : {"GET /api/v1/libobs/catalog", "GET /api/v1/libobs/scenes", "PUT /api/v1/libobs/scenes", "POST /api/v1/libobs/scenes",
                              "DELETE /api/v1/libobs/scenes/{id}", "POST /api/v1/libobs/validate", "POST /api/v1/channels/{id}/libobs/show",
                              "POST /api/v1/channels/{id}/libobs/switch", "POST /api/v1/channels/{id}/libobs/hide", "POST /api/v1/channels/{id}/libobs/filter", "GET /api/v1/channels/{id}/libobs/status"})
            QVERIFY2(keys.contains(QLatin1String(k)), k);
        RestApiServer station(nullptr); station.copyRoutes(*f.rest);
        QCOMPARE(station.routeKeys().filter(S("/libobs")).size(), 11);
    }

    void catalogShape() {
        const QJsonObject c = libobsroutes::catalog();
        QVERIFY(c.value("items").toArray().size() >= 6);
        QVERIFY(c.value("filters").toArray().size() >= 6);
        QCOMPARE(c.value("transitions").toArray().size(), 7);
        QCOMPARE(c.value("layerDefault").toInt(), 50);
        QStringList ids; for (const QJsonValue& v : c.value("transitions").toArray()) ids << v.toObject().value("id").toString();
        for (const char* t : {"cut_transition", "fade_transition", "fade_to_color_transition", "swipe_transition", "slide_transition", "wipe_transition", "obs_stinger_transition"}) QVERIFY2(ids.contains(QLatin1String(t)), t);
        // every kind carries params[] with key/type, the stinger its path
        bool stingerPath = false;
        for (const QJsonValue& v : c.value("transitions").toArray()) { const QJsonObject k = v.toObject(); QVERIFY(k.value("params").isArray()); for (const QJsonValue& p : k.value("params").toArray()) { QVERIFY(!p.toObject().value("key").toString().isEmpty()); QVERIFY(!p.toObject().value("type").toString().isEmpty()); if (k.value("id") == "obs_stinger_transition" && p.toObject().value("key") == "path") stingerPath = true; } }
        QVERIFY(stingerPath);
        Fixture f; QVERIFY(f.init());
        const HttpReply r = http(f.port(), "GET", "/api/v1/libobs/catalog");
        QCOMPARE(r.status, 200);
        QCOMPARE(json(r.body).value("transitions").toArray().size(), 7);
    }

    void validateAccepts() {
        QJsonObject n;
        QCOMPARE(libobsroutes::validateSpec(exampleSpec(), &n), QString());
        QCOMPARE(n.value("type").toString(), S("libobs"));
        QCOMPARE(n.value("program").toString(), S("A"));
        QCOMPARE(n.value("transition").toObject().value("durationMs").toInt(), 500);
        // defaults: program = first scene, transition = cut, item names generated, string transition accepted
        QJsonObject m = json(R"({"scenes":[{"items":[{"id":"random"},{"id":"random"}]}],"transition":"fade_transition"})");
        QCOMPARE(libobsroutes::validateSpec(m, &n), QString());
        QCOMPARE(n.value("program").toString(), S("scene1"));
        QCOMPARE(n.value("transition").toObject().value("id").toString(), S("fade_transition"));
        QCOMPARE(n.value("transition").toObject().value("durationMs").toInt(), 500);
        const QJsonArray items = n.value("scenes").toArray().first().toObject().value("items").toArray();
        QCOMPARE(items.size(), 2);
        QVERIFY(items[0].toObject().value("name").toString() != items[1].toObject().value("name").toString());
    }

    void validateRefuses() {
        auto bad = [](const char* js) { return libobsroutes::validateSpec(json(js), nullptr); };
        QVERIFY(bad(R"({})").contains("at least one scene"));
        QVERIFY(bad(R"({"scenes":[{"name":"A","items":[{"id":"browser_source"}]}]})").contains("unknown item kind"));
        QVERIFY(bad(R"({"scenes":[{"name":"A","items":[{"id":"random","filters":[{"id":"nope"}]}]}]})").contains("unknown filter"));
        QVERIFY(bad(R"({"scenes":[{"name":"A"},{"name":"A"}]})").contains("duplicate scene"));
        QVERIFY(bad(R"({"scenes":[{"name":"A"}],"program":"Z"})").contains("not a scene"));
        QVERIFY(bad(R"({"scenes":[{"name":"A"}],"transition":"dissolve"})").contains("unknown transition"));
        QVERIFY(bad(R"({"scenes":[{"name":"A"}],"transition":{"id":"fade_transition","durationMs":99999}})").contains("durationMs"));
        QVERIFY(bad(R"({"scenes":[{"name":"A","items":[{"id":"random","fit":"cover"}]}]})").contains("fit"));
        QVERIFY(bad(R"({"scenes":[{"name":"A","items":[{"id":"random","name":"n"},{"id":"random","name":"n"}]}]})").contains("duplicate item"));
        QVERIFY(bad(R"({"type":"ndi","scenes":[{"name":"A"}]})").contains("type"));
        QVERIFY(bad(R"({"scenes":[{"name":"A","items":[{"id":"random","settings":5}]}]})").contains("settings"));
        // saved records
        QJsonObject rec;
        QVERIFY(libobsroutes::validateSaved(json(R"({"name":"","spec":{"scenes":[{"name":"A"}]}})"), &rec).contains("name"));
        QVERIFY(libobsroutes::validateSaved(json(R"({"id":"bad id!","name":"x","spec":{"scenes":[{"name":"A"}]}})"), &rec).contains("scene id"));
        QCOMPARE(libobsroutes::validateSaved(json(R"({"name":"Studio Wide","spec":{"scenes":[{"name":"A"}]}})"), &rec), QString());
        QCOMPARE(rec.value("id").toString(), S("studio-wide"));
        QJsonArray list;
        QVERIFY(libobsroutes::validateList(QJsonArray{rec, rec}, &list).contains("duplicate scene id"));
    }

    void commandBuilders() {
        QJsonObject n; QCOMPARE(libobsroutes::validateSpec(exampleSpec(), &n), QString());
        const QJsonObject show = libobsroutes::showCommand(n, 50, QJsonObject());
        QCOMPARE(show.value("action").toString(), S("show"));
        QCOMPARE(show.value("layer").toInt(), 50);
        QCOMPARE(show.value("source").toObject().value("type").toString(), S("libobs"));
        QCOMPARE(show.value("source").toObject().value("scenes").toArray().size(), 2);
        QCOMPARE(show.value("in").toObject().value("type").toString(), S("cut"));
        QJsonObject tr; tr["id"] = "obs_stinger_transition"; tr["durationMs"] = 800;
        const QJsonObject sw = libobsroutes::switchCommand(50, "B", tr);
        QCOMPARE(sw.value("action").toString(), S("update"));
        QCOMPARE(sw.value("switch").toString(), S("B"));
        QCOMPARE(sw.value("transition").toObject().value("id").toString(), S("obs_stinger_transition"));
        QCOMPARE(libobsroutes::hideCommand(50, true, 0.3).value("action").toString(), S("remove"));
        const QJsonObject h = libobsroutes::hideCommand(50, false, 0.3);
        QCOMPARE(h.value("action").toString(), S("hide"));
        QCOMPARE(h.value("transition").toObject().value("type").toString(), S("fade"));
        // telemetry pick: the libobs layer among others, preferring the remembered id
        const QJsonObject state = json(R"({"layers":[{"id":30,"state":"shown","source":{"type":"html"}},{"id":50,"state":"shown","source":{"type":"libobs"},"sourceStatus":{"obs":{"program":"A"}}}]})");
        QCOMPARE(libobsroutes::layerFromTelemetry(state, 50).value("sourceStatus").toObject().value("obs").toObject().value("program").toString(), S("A"));
        QVERIFY(libobsroutes::layerFromTelemetry(json(R"({"layers":[{"id":30,"source":{"type":"html"}}]})"), 50).isEmpty());
    }

    void sceneStoreOverTheWire() {
        Fixture f; QVERIFY(f.init());
        QCOMPARE(json(http(f.port(), "GET", "/api/v1/libobs/scenes").body).value("scenes").toArray().size(), 0);
        QJsonObject rec; rec["name"] = "Studio A"; rec["spec"] = exampleSpec();
        QJsonObject list; list["scenes"] = QJsonArray{rec};
        HttpReply r = http(f.port(), "PUT", "/api/v1/libobs/scenes", bytes(list));
        QCOMPARE(r.status, 200);
        QCOMPARE(json(r.body).value("scenes").toArray().first().toObject().value("id").toString(), S("studio-a"));
        QCOMPARE(f.store.settingJson("libobs.scenes").value("scenes").toArray().size(), 1);
        QVERIFY(!f.broadcasts.isEmpty()); QCOMPARE(f.broadcasts.last().value("type").toString(), S("libobsScenes"));
        // a bad list is refused whole and leaves the store alone
        QJsonObject badRec = rec; QJsonObject badSpec = exampleSpec(); badSpec["program"] = "Z"; badRec["spec"] = badSpec; badRec["name"] = "Bad";
        list["scenes"] = QJsonArray{rec, badRec};
        r = http(f.port(), "PUT", "/api/v1/libobs/scenes", bytes(list));
        QCOMPARE(r.status, 422);
        QVERIFY(json(r.body).value("error").toString().contains("not a scene"));
        QCOMPARE(f.store.settingJson("libobs.scenes").value("scenes").toArray().size(), 1);
        // upsert one (same id replaces), then another, then delete
        rec["layer"] = 51; rec["id"] = "studio-a";
        r = http(f.port(), "POST", "/api/v1/libobs/scenes", bytes(rec));
        QCOMPARE(r.status, 200); QVERIFY(json(r.body).value("replaced").toBool());
        QJsonObject rec2 = rec; rec2["id"] = "b"; rec2["name"] = "B";
        QCOMPARE(http(f.port(), "POST", "/api/v1/libobs/scenes", bytes(rec2)).status, 200);
        QCOMPARE(json(http(f.port(), "GET", "/api/v1/libobs/scenes").body).value("scenes").toArray().size(), 2);
        QCOMPARE(http(f.port(), "DELETE", "/api/v1/libobs/scenes/b").status, 200);
        QCOMPARE(http(f.port(), "DELETE", "/api/v1/libobs/scenes/b").status, 404);
        const QJsonArray left = json(http(f.port(), "GET", "/api/v1/libobs/scenes").body).value("scenes").toArray();
        QCOMPARE(left.size(), 1);
        QCOMPARE(left.first().toObject().value("layer").toInt(), 51);
        // validate (dry run) answers the command without touching anything
        QJsonObject v; v["spec"] = exampleSpec(); v["layer"] = 60;
        const QJsonObject dry = json(http(f.port(), "POST", "/api/v1/libobs/validate", bytes(v)).body);
        QVERIFY(dry.value("ok").toBool());
        QCOMPARE(dry.value("command").toObject().value("layer").toInt(), 60);
    }

    void channelRoutesWithoutLoopback() {
        Fixture f; QVERIFY(f.init());
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/filter", bytes(json(R"({"item":"subject","filter":{"id":"chroma_key_filter_v2","settings":{"smoothness":1001}}})"))).status, 422);
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/filter", bytes(json(R"({"item":"subject","filter":{"id":"chroma_key_filter_v2","settings":{"key_color":"invalid"}}})"))).status, 422);
        QJsonObject b; b["sceneId"] = "nope";
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/show", bytes(b)).status, 404);
        b = QJsonObject(); b["spec"] = json(R"({"scenes":[{"name":"A","items":[{"id":"nope"}]}]})");
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/show", bytes(b)).status, 422);
        b = QJsonObject(); b["spec"] = exampleSpec(); b["layer"] = 0;
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/show", bytes(b)).status, 422);
        // a good spec with restPort 0: refused at the loopback step, never an engine
        b = QJsonObject(); b["spec"] = exampleSpec();
        HttpReply r = http(f.port(), "POST", "/api/v1/channels/ch1/libobs/show", bytes(b));
        QCOMPARE(r.status, 503); QVERIFY(json(r.body).value("error").toString().contains("loopback"));
        b = QJsonObject(); QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/switch", bytes(b)).status, 422);
        b["to"] = "B"; b["transition"] = "dissolve"; QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/switch", bytes(b)).status, 422);
        b["transition"] = "fade_transition"; b["durationMs"] = 300; QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/switch", bytes(b)).status, 503);
        QCOMPARE(http(f.port(), "POST", "/api/v1/channels/ch1/libobs/hide", bytes(QJsonObject())).status, 503);
        // status still answers (200) and says the state is unreachable
        r = http(f.port(), "GET", "/api/v1/channels/ch1/libobs/status");
        QCOMPARE(r.status, 200);
        QCOMPARE(json(r.body).value("engineState").toString(), S("UNREACHABLE"));
        QCOMPARE(json(r.body).value("onAir").toBool(), false);
    }
};

QTEST_MAIN(TestLibObsRoutes)
#include "test_libobsroutes.moc"
