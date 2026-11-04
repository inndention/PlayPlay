// Now Playing lane tests (docs/nowplaying.md): playout/supervisor/nowplayingroutes.cpp's laneext registrar driven the way
// SupervisorCore drives it (laneext::installAll on a Context) against a real RestApiServer on an ephemeral loopback port.
// The same server also serves stub versions of the upstream routes the bundle is assembled from (/channels/{id}, /state,
// /runlist, /overlays/{ch}, /overlays/cgslots, /system/stats, /live/facebook), so the module's loopback fetches land here.
// Covers: registration; the route table (and the station copy); GET /api/v1/nowplaying/{id} over the wire merges every part,
// the switcher map comes from the store's ui.inputMap setting (shared with the desktop InputMap) with the engine's monitor state
// per key, the on-air slot is found by engine path, per-input snapshot URLs point at the Supervisor's preview store, a failed
// upstream part is named in "errors" without failing the bundle, an unknown channel answers 404, concurrent requests are
// coalesced and a fresh bundle is served from the cache. Nothing here touches the live stack (port 0, temp dirs only).
#include <QtTest>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include "../playout/schedule/schedulestore.h"
#include "../playout/supervisor/laneext.h"
#include "../playout/supervisor/restapiserver.h"

namespace nowplaying {
QJsonObject assemble(const QString& channelId, const QJsonObject& channel, const QJsonObject& state, const QJsonObject& runlist,
                     const QJsonObject& overlays, const QJsonObject& cgslots, const QJsonObject& stats, const QJsonObject& facebook,
                     const QJsonObject& inputMap, const QStringList& errors);
}

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }
using Q = QHash<QString, QString>;

struct HttpReply { int status = 0; QJsonObject body; QByteArray raw; };

/// One GET through QNetworkAccessManager, spinning the event loop (the server and the module's own loopback fetches run on it).
HttpReply get(quint16 port, const QString& path, int timeoutMs = 8000) {
    static QNetworkAccessManager nam;
    HttpReply out;
    QNetworkReply* r = nam.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path))));
    QEventLoop loop; QTimer t; t.setSingleShot(true);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(timeoutMs); loop.exec();
    out.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.raw = r->readAll(); out.body = QJsonDocument::fromJson(out.raw).object();
    r->deleteLater();
    return out;
}

struct Fixture {
    QTemporaryDir dir;
    sched::ScheduleStore store;
    std::unique_ptr<RestApiServer> rest;
    QObject parent;
    laneext::Context ctx;
    int stateHits = 0;           // how often the stub /state answered (coalescing / cache checks)
    bool overlaysFail = false;   // stub /overlays/{ch} answers 500

    bool init() {
        if (!dir.isValid() || !store.open(dir.filePath(S("sup.db")))) return false;
        sched::ChannelProfile cp; cp.id = S("np1"); cp.name = S("Now Playing One"); cp.timezone = S("UTC");
        if (!store.saveChannel(cp)) return false;
        rest = std::make_unique<RestApiServer>(nullptr);
        if (!rest->start(0)) return false;
        stubs();
        ctx.parent = &parent; ctx.rest = rest.get(); ctx.store = &store; ctx.dataDir = dir.path();
        ctx.restPort = rest->port();
        ctx.restToken = []() { return QString(); };
        return true;
    }
    quint16 port() const { return rest->port(); }

    void stubs() {
        rest->registerRoute("GET", "/api/v1/channels/{id}", [](const QJsonObject&, const Q& q) {
            QJsonObject o; if (q["id"] != "np1") { o["error"] = "no such channel"; o["status"] = 404; return o; }
            o["profile"] = QJsonObject{{"id", "np1"}, {"name", "Now Playing One"}, {"callLetters", "WNPO"}, {"width", 1920}, {"height", 1080}, {"frameRate", "30000/1001"}};
            o["process"] = QJsonObject{{"connected", true}}; o["executor"] = QJsonObject{{"armed", true}, {"lastPushReason", "hour"}}; o["engineState"] = "ON_AIR"; return o;
        });
        rest->registerRoute("GET", "/api/v1/channels/{id}/state", [this](const QJsonObject&, const Q&) {
            ++stateHits;
            QJsonObject o; o["state"] = "ON_AIR"; o["itemId"] = "r2"; o["title"] = "Studio cam"; o["path"] = "ndi://DESK (Cam 1)"; o["nextItemId"] = "r3"; o["nextTitle"] = "Promo"; o["nextReady"] = true;
            o["position"] = 12.5; o["duration"] = 1800; o["remaining"] = 1787.5; o["timecode"] = "00:00:12:15";
            o["monitors"] = QJsonArray{QJsonObject{{"key", "in1"}, {"state", "open"}, {"connected", true}, {"warm", true}, {"frames", 40}}, QJsonObject{{"key", "in3"}, {"state", "failed"}, {"error", "no such source"}}};
            o["layers"] = QJsonArray{QJsonObject{{"id", 4}, {"role", "lower_third"}, {"state", "shown"}}};
            o["compositor"] = QJsonObject{{"audio", QJsonObject{{"momentaryLufs", -23.4}}}};
            o["outputs"] = QJsonArray{QJsonObject{{"name", "udp-main"}, {"state", "running"}}};
            return o;
        });
        rest->registerRoute("GET", "/api/v1/channels/{id}/runlist", [](const QJsonObject&, const Q&) {
            QJsonObject o; o["current"] = QJsonObject{{"hourStartMs", 1000.0}, {"items", QJsonArray{
                QJsonObject{{"id", "r1"}, {"title", "News"}, {"type", "program"}, {"duration", 600}, {"plannedStartMs", 1000.0}, {"plannedEndMs", 601000.0}},
                QJsonObject{{"id", "r2"}, {"title", "Studio cam"}, {"type", "live"}, {"duration", 1800}, {"plannedStartMs", 601000.0}, {"plannedEndMs", 2401000.0}, {"mediaId", "m9"}},
                QJsonObject{{"id", "r3"}, {"title", "Promo"}, {"type", "promo"}, {"duration", 30}, {"plannedStartMs", 2401000.0}}}}};
            o["next"] = QJsonObject{{"items", QJsonArray{}}}; return o;
        });
        rest->registerRoute("GET", "/api/v1/overlays/{ch}", [this](const QJsonObject&, const Q&) {
            QJsonObject o; if (overlaysFail) { o["error"] = "overlay manager busy"; o["status"] = 500; return o; }
            o["layers"] = QJsonArray{QJsonObject{{"layer", 4}, {"role", "lower_third"}, {"template", "lower_third_modern"}, {"label", "L3"}, {"state", "shown"}, {"params", QJsonObject{{"text1", "Jane Doe"}}}}};
            o["item"] = QJsonObject{{"active", true}}; return o;
        });
        rest->registerRoute("GET", "/api/v1/overlays/cgslots", [](const QJsonObject&, const Q&) { QJsonObject o; o["slots"] = QJsonArray{QJsonObject{{"id", "cg1"}, {"name", "CG1"}, {"template", "bug_live_indicator"}, {"inputs", QJsonArray{"*"}}}}; return o; });
        rest->registerRoute("GET", "/api/v1/system/stats", [](const QJsonObject&, const Q&) {
            QJsonObject o; o["cpu"] = QJsonObject{{"systemPct", 31.5}}; o["memory"] = QJsonObject{{"usedPct", 42}}; o["net"] = QJsonObject{{"inBps", 1000}, {"outBps", 2000}};
            o["channels"] = QJsonArray{QJsonObject{{"id", "other"}, {"fps", 25.0}}, QJsonObject{{"id", "np1"}, {"fps", 29.97}, {"bitrateKbps", 8000}}}; return o;
        });
        rest->registerRoute("GET", "/api/v1/live/facebook", [](const QJsonObject&, const Q&) { QJsonObject o; o["channel"] = "np1"; o["hasKey"] = true; o["keyHint"] = "…abcd"; o["state"] = "idle"; o["live"] = false; return o; });
    }
};

} // namespace

class TestNowPlaying : public QObject {
    Q_OBJECT
private slots:
    void registered() { QVERIFY(laneext::registered().contains(S("nowplaying"))); }

    void installAllMountsRoutes() {
        Fixture f; QVERIFY(f.init());
        QVERIFY(f.rest->routeKeys().filter(S("/nowplaying")).isEmpty());
        const QStringList installed = laneext::installAll(f.ctx);
        QVERIFY2(installed.contains(S("nowplaying")), qPrintable(installed.join(',')));
        const QStringList keys = f.rest->routeKeys();
        QVERIFY(keys.contains(S("GET /api/v1/nowplaying")));
        QVERIFY(keys.contains(S("GET /api/v1/nowplaying/{id}")));
        RestApiServer station(nullptr); station.copyRoutes(*f.rest);
        QCOMPARE(station.routeKeys().filter(S("/api/v1/nowplaying")).size(), 2);
        const HttpReply info = get(f.port(), S("/api/v1/nowplaying"));
        QCOMPARE(info.status, 200);
        QCOMPARE(info.body.value("lane").toString(), S("nowplaying"));
        QVERIFY(info.body.value("snapshots").toString().contains(S("/preview/")));
    }

    void bundleOverTheWire() {
        Fixture f; QVERIFY(f.init());
        // The switcher map the desktop InputMap saves: 8 slots (two mapped), the AUTO transition.
        const QJsonObject map{{"slots", QJsonArray{QJsonObject{{"kind", "ndi"}, {"title", "Cam 1"}, {"path", "live://ndi/DESK (Cam 1)"}, {"mediaId", ""}, {"enginePath", "ndi://DESK (Cam 1)/"}, {"sub", ""}},
                                                   QJsonValue(), QJsonObject{{"kind", "camera"}, {"title", "Lobby"}, {"path", "live://stream/rtsp://10.0.0.9/live"}, {"enginePath", "rtsp://10.0.0.9/live"}}}},
                              {"transition", QJsonObject{{"type", "dip"}, {"durationSec", 1.0}}}};
        QVERIFY(f.store.setSetting(S("ui.inputMap"), QString::fromUtf8(QJsonDocument(map).toJson(QJsonDocument::Compact))));
        laneext::installAll(f.ctx);
        const HttpReply r = get(f.port(), S("/api/v1/nowplaying/np1"));
        QCOMPARE(r.status, 200);
        const QJsonObject b = r.body;
        QVERIFY2(b.value("ok").toBool(), r.raw.left(300).constData());
        QCOMPARE(b.value("errors").toArray().size(), 0);
        // channel + state
        const QJsonObject ch = b.value("channel").toObject();
        QCOMPARE(ch.value("name").toString(), S("Now Playing One")); QVERIFY(ch.value("engineConnected").toBool()); QVERIFY(ch.value("armed").toBool());
        QCOMPARE(b.value("state").toObject().value("state").toString(), S("ON_AIR"));
        QCOMPARE(b.value("state").toObject().value("timecode").toString(), S("00:00:12:15"));
        // on air enriched from the runlist; next
        const QJsonObject on = b.value("onAir").toObject();
        QCOMPARE(on.value("title").toString(), S("Studio cam")); QCOMPARE(on.value("type").toString(), S("live")); QVERIFY(on.value("live").toBool());
        QCOMPARE(on.value("plannedEndMs").toDouble(), 2401000.0); QCOMPARE(on.value("mediaId").toString(), S("m9"));
        const QJsonObject nx = b.value("next").toObject();
        QCOMPARE(nx.value("title").toString(), S("Promo")); QVERIFY(nx.value("ready").toBool()); QCOMPARE(nx.value("type").toString(), S("promo"));
        QCOMPARE(b.value("runlist").toObject().value("current").toObject().value("items").toArray().size(), 3);
        // inputs: 8 slots, map shared with the desktop, monitors merged, on-air slot by engine path, snapshot URLs
        const QJsonObject in = b.value("inputs").toObject();
        QCOMPARE(in.value("count").toInt(), 8);
        const QJsonArray slotList = in.value("slots").toArray();
        QCOMPARE(slotList.size(), 8);
        const QJsonObject s0 = slotList.at(0).toObject(), s1 = slotList.at(1).toObject(), s2 = slotList.at(2).toObject();
        QVERIFY(s0.value("mapped").toBool()); QCOMPARE(s0.value("title").toString(), S("Cam 1")); QCOMPARE(s0.value("key").toString(), S("in1"));
        QVERIFY2(s0.value("onAir").toBool(), "slot 1 engine path matches what the engine plays (case / trailing slash aside)");
        QCOMPARE(s0.value("snapshotUrl").toString(), S("/preview/np1:in1.jpg"));
        QCOMPARE(s0.value("monitor").toObject().value("state").toString(), S("open"));
        QVERIFY(!s1.value("mapped").toBool()); QVERIFY(!s1.contains("snapshotUrl"));
        QVERIFY(s2.value("mapped").toBool()); QVERIFY(!s2.value("onAir").toBool()); QCOMPARE(s2.value("monitor").toObject().value("error").toString(), S("no such source"));
        QCOMPARE(in.value("onAirIndex").toInt(), 0);
        QCOMPARE(in.value("transition").toObject().value("type").toString(), S("dip"));
        QVERIFY(in.value("mapLoaded").toBool());
        // graphics, meters, machine, facebook
        const QJsonObject cg = b.value("cg").toObject();
        QCOMPARE(cg.value("layers").toArray().size(), 1); QCOMPARE(cg.value("layers").toArray().at(0).toObject().value("label").toString(), S("L3"));
        QCOMPARE(cg.value("engineLayers").toArray().size(), 1); QCOMPARE(cg.value("slots").toArray().size(), 1);
        QCOMPARE(b.value("audio").toObject().value("momentaryLufs").toDouble(), -23.4);
        QCOMPARE(b.value("outputs").toArray().size(), 1);
        QCOMPARE(b.value("stats").toObject().value("cpu").toObject().value("systemPct").toDouble(), 31.5);
        QCOMPARE(b.value("channelStats").toObject().value("fps").toDouble(), 29.97);
        QCOMPARE(b.value("previewUrl").toString(), S("/preview/np1.jpg"));
        QVERIFY(b.value("facebook").toObject().value("hasKey").toBool());
    }

    void cacheAndCoalescing() {
        Fixture f; QVERIFY(f.init()); laneext::installAll(f.ctx);
        QCOMPARE(get(f.port(), S("/api/v1/nowplaying/np1")).status, 200);
        const int hits = f.stateHits;
        QCOMPARE(hits, 1);
        const HttpReply again = get(f.port(), S("/api/v1/nowplaying/np1"));     // within the cache window: no new upstream fetch
        QCOMPARE(again.status, 200);
        QVERIFY(again.body.value("cached").toBool());
        QCOMPARE(f.stateHits, hits);
        // Two requests fired together after the cache lapses share one assembly.
        QTest::qWait(450);
        QNetworkAccessManager nam; QEventLoop loop; int done = 0;
        QNetworkReply* a = nam.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/nowplaying/np1").arg(f.port()))));
        QNetworkReply* c = nam.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/nowplaying/np1").arg(f.port()))));
        auto fin = [&]() { if (++done == 2) loop.quit(); };
        connect(a, &QNetworkReply::finished, &loop, fin); connect(c, &QNetworkReply::finished, &loop, fin);
        QTimer::singleShot(8000, &loop, &QEventLoop::quit); loop.exec();
        QCOMPARE(done, 2);
        QCOMPARE(a->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(c->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY2(f.stateHits <= hits + 2, qPrintable(QString::number(f.stateHits)));   // one assembly (or one + a cache miss), never one per client per second
        a->deleteLater(); c->deleteLater();
    }

    void partialFailureAndUnknownChannel() {
        Fixture f; QVERIFY(f.init()); f.overlaysFail = true; laneext::installAll(f.ctx);
        const HttpReply r = get(f.port(), S("/api/v1/nowplaying/np1"));
        QCOMPARE(r.status, 200);
        const QJsonArray errs = r.body.value("errors").toArray();
        QCOMPARE(errs.size(), 1);
        QVERIFY2(errs.at(0).toString().startsWith(S("overlays:")), qPrintable(errs.at(0).toString()));
        QCOMPARE(r.body.value("cg").toObject().value("layers").toArray().size(), 0);          // the part is simply empty
        QCOMPARE(r.body.value("cg").toObject().value("engineLayers").toArray().size(), 1);    // the engine's own layers still came
        QCOMPARE(get(f.port(), S("/api/v1/nowplaying/nope")).status, 404);
        QCOMPARE(get(f.port(), S("/api/v1/nowplaying/bad%20id%21")).status, 400);
    }

    void assembleWithoutMap() {
        // No ui.inputMap saved yet: eight unmapped slots, the default AUTO transition, nothing on air.
        const QJsonObject b = nowplaying::assemble(S("x"), QJsonObject{{"profile", QJsonObject{{"id", "x"}}}}, QJsonObject{{"state", "IDLE"}}, {}, {}, {}, {}, {}, {}, {});
        const QJsonObject in = b.value("inputs").toObject();
        QCOMPARE(in.value("slots").toArray().size(), 8);
        QVERIFY(!in.value("mapLoaded").toBool());
        QCOMPARE(in.value("onAirIndex").toInt(), -1);
        QCOMPARE(in.value("transition").toObject().value("type").toString(), S("mix"));
        QVERIFY(b.value("channel").toObject().value("found").toBool());
        QCOMPARE(b.value("state").toObject().value("state").toString(), S("IDLE"));
    }
};

QTEST_MAIN(TestNowPlaying)
#include "test_nowplaying.moc"
