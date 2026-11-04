// Unit tests for the reporter feed (playout/supervisor/reporterfeed.*): session store, state machine, countdown ticks,
// write-once stream key, port slots, URLs and the ffmpeg listener command line. Every hook is a stub: no process, no network.
#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include "playout/supervisor/reporterfeed.h"

using namespace reporterfeed;

class TestReporterFeed : public QObject {
    Q_OBJECT
private:
    struct Harness {
        ReporterFeed f;
        QStringList ingestStarted, ingestStopped, inputsSaved, inputsRemoved, takes, releases, events;
        QHash<QString, QString> keysSeen;      // id -> key handed to startIngest
        bool takeOk = true;
        qint64 now = 1700000000000;
        Harness() {
            f.config.host = "10.0.0.5";
            f.nowMs = [this] { return now; };
            f.startIngest = [this](const Session& s, const QString& k) { ingestStarted << s.id; keysSeen[s.id] = k; };
            f.stopIngest = [this](const Session& s) { ingestStopped << s.id; };
            f.saveInput = [this](const Session& s) { inputsSaved << s.id; };
            f.removeInput = [this](const Session& s) { inputsRemoved << s.id; };
            f.takeToProgram = [this](const Session& s, QString* why) { takes << s.id; if (!takeOk && why) *why = "engine down"; return takeOk; };
            f.releaseProgram = [this](const Session& s) { releases << s.id; };
            f.broadcast = [this](const QString& ev, const QJsonObject& p) { events << ev + ":" + QString::number(p.value("remainingSec").toInt()); };
        }
        QJsonObject create(const QString& who, const QString& ch = "main", int cd = 3) {
            QJsonObject b; b["reporter"] = who; b["channel"] = ch; b["countdownSec"] = cd; QString err;
            const QJsonObject o = f.create(b, &err); if (o.isEmpty()) qWarning() << err; return o;
        }
    };

private slots:
    void createGivesKeyOnce() {
        Harness h;
        const QJsonObject o = h.create("Jane Doe");
        const QString id = o.value("id").toString(), key = o.value("streamKey").toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(key.size(), 24);
        QCOMPARE(o.value("publishUrl").toString(), QString("rtmp://10.0.0.5:1936/reporter/") + key);
        QCOMPARE(o.value("enginePath").toString(), QString("udp://127.0.0.1:50040"));
        QCOMPARE(o.value("status").toString(), QString("idle"));
        // the key never comes back: not in the session, the list, the inputs or the ffmpeg args' public parts
        const QJsonObject s = h.f.session(id).toJson();
        QVERIFY(!s.contains("streamKey")); QVERIFY(!s.contains("publishUrl"));
        QCOMPARE(s.value("keyHint").toString(), ReporterFeed::hashKey(key).left(8));
        QVERIFY(!QJsonDocument(h.f.sessionsJson()).toJson().contains(key.toUtf8()));
        QVERIFY(!QJsonDocument(h.f.inputsJson()).toJson().contains(key.toUtf8()));
        QCOMPARE(h.keysSeen.value(id), key);           // the listener got it
        QCOMPARE(h.ingestStarted, QStringList{id}); QCOMPARE(h.inputsSaved, QStringList{id});
        QCOMPARE(h.events, QStringList{"session:0"});
    }

    void createValidates() {
        Harness h; QString err;
        QVERIFY(h.f.create(QJsonObject{{"reporter", "  "}}, &err).isEmpty()); QVERIFY(err.contains("reporter"));
        const QJsonObject o = h.f.create(QJsonObject{{"reporter", "X"}, {"countdownSec", 9999}}, &err);
        QCOMPARE(o.value("countdownSec").toInt(), 600);
        QCOMPARE(o.value("title").toString(), QString("LIVE"));
        // the legacy snake_case countdown is read too
        const QJsonObject l = h.f.create(QJsonObject{{"reporter", "Y"}, {"countdown_sec", 7.0}, {"channel_id", "ch2"}}, &err);
        QCOMPARE(l.value("countdownSec").toInt(), 7); QCOMPARE(l.value("channel").toString(), QString("ch2"));
    }

    void inputsListActiveSessions() {
        Harness h;
        const QString a = h.create("Ann").value("id").toString(), b = h.create("Bob").value("id").toString();
        QJsonArray in = h.f.inputsJson();
        QCOMPARE(in.size(), 2);
        QCOMPARE(in[0].toObject().value("name").toString(), QString("Reporter: Ann"));
        QCOMPARE(in[0].toObject().value("type").toString(), QString("stream"));
        QCOMPARE(in[0].toObject().value("path").toString(), "live://reporter/" + a);
        QCOMPARE(in[1].toObject().value("url").toString(), QString("udp://127.0.0.1:50041"));   // second slot
        QVERIFY(h.f.end(a, nullptr));
        in = h.f.inputsJson();
        QCOMPARE(in.size(), 1); QCOMPARE(in[0].toObject().value("sessionId").toString(), b);
        QCOMPARE(h.f.sessionsJson(false).size(), 1); QCOMPARE(h.f.sessionsJson(true).size(), 2);
    }

    void slotsAreReusedAfterEnd() {
        Harness h; h.f.config.portCount = 2;
        const QString a = h.create("A").value("id").toString(), b = h.create("B").value("id").toString();
        QCOMPARE(h.f.session(a).slot, 0); QCOMPARE(h.f.session(b).slot, 1);
        QString err; QVERIFY(h.f.create(QJsonObject{{"reporter", "C"}}, &err).isEmpty()); QVERIFY(err.contains("ports"));
        QVERIFY(h.f.end(a, nullptr));
        const QJsonObject c = h.create("C");
        QCOMPARE(c.value("slot").toInt(), 0);
        QCOMPARE(c.value("publishUrl").toString().mid(0, 20), QString("rtmp://10.0.0.5:1936"));
    }

    void countdownTakesAtZero() {
        Harness h;
        const QString id = h.create("Jane", "main", 3).value("id").toString();
        h.events.clear();
        QString err; QVERIFY(h.f.go(id, -1, &err));
        QCOMPARE(h.f.session(id).status, QString("countdown")); QCOMPARE(h.f.session(id).remainingSec, 3);
        QVERIFY(h.f.go(id, -1, &err));                 // a second GO while counting is ignored
        h.f.tick(); QCOMPARE(h.f.session(id).remainingSec, 2); QVERIFY(h.takes.isEmpty());
        h.f.tick(); QCOMPARE(h.f.session(id).remainingSec, 1); QVERIFY(h.takes.isEmpty());
        h.now += 1000;
        h.f.tick();
        QCOMPARE(h.takes, QStringList{id});
        QCOMPARE(h.f.session(id).status, QString("live")); QCOMPARE(h.f.session(id).remainingSec, 0);
        QCOMPARE(h.f.session(id).liveMs, h.now);
        QCOMPARE(h.events, (QStringList{"countdown:3", "countdown:2", "countdown:1", "live:0"}));
        h.f.tick(); QCOMPARE(h.takes.size(), 1);       // live sessions are not ticked again
    }

    void goWithExplicitCountdownAndZero() {
        Harness h;
        const QString id = h.create("Jane", "main", 10).value("id").toString();
        QString err; QVERIFY(h.f.go(id, 0, &err));     // zero = take now
        QCOMPARE(h.f.session(id).status, QString("live")); QCOMPARE(h.takes.size(), 1);
        const QString id2 = h.create("Jim", "main", 10).value("id").toString();
        QVERIFY(h.f.go(id2, 2, &err)); QCOMPARE(h.f.session(id2).countdownSec, 2); QCOMPARE(h.f.session(id2).remainingSec, 2);
    }

    void cancelAndTakeNow() {
        Harness h;
        const QString id = h.create("Jane", "main", 5).value("id").toString();
        QString err;
        QVERIFY(!h.f.cancel(id, &err));                // nothing to cancel while idle
        QVERIFY(h.f.go(id, -1, &err)); h.f.tick();
        QVERIFY(h.f.cancel(id, &err));
        QCOMPARE(h.f.session(id).status, QString("idle")); QCOMPARE(h.f.session(id).remainingSec, 0);
        h.f.tick(); QVERIFY(h.takes.isEmpty());        // a cancelled countdown never takes
        QVERIFY(h.f.go(id, -1, &err)); QVERIFY(h.f.takeNow(id, &err));
        QCOMPARE(h.f.session(id).status, QString("live")); QCOMPARE(h.takes, QStringList{id});
        QVERIFY(h.f.takeNow(id, &err));                // idempotent while live
        QCOMPARE(h.takes.size(), 1);
    }

    void refusedTakeFallsBackToIdle() {
        Harness h; h.takeOk = false;
        const QString id = h.create("Jane", "main", 1).value("id").toString();
        QString err; QVERIFY(h.f.go(id, -1, &err)); h.f.tick();
        QCOMPARE(h.f.session(id).status, QString("idle"));
        QCOMPARE(h.f.session(id).lastError, QString("engine down"));
        QVERIFY(!h.f.takeNow(id, &err)); QCOMPARE(err, QString("engine down"));
    }

    void asynchronousTakeWaitsForReply() {
        Harness h;
        std::function<void(bool, const QString&)> reply;
        h.f.takeToProgramAsync = [&](const Session&, std::function<void(bool, const QString&)> done) { reply = done; };
        const QString id = h.create("Jane", "main", 0).value("id").toString();
        QString err; QVERIFY(!h.f.go(id, 0, &err));
        QCOMPARE(h.f.session(id).lastError, QString("reporter feed is not publishing"));
        h.f.noteFeed(id, true);
        QVERIFY(h.f.go(id, 0, &err));
        QCOMPARE(h.f.session(id).status, QString("taking"));
        QVERIFY(!h.f.end(id, &err)); QVERIFY(!h.f.remove(id));
        reply(false, "engine offline");
        QCOMPARE(h.f.session(id).status, QString("idle"));
        QCOMPARE(h.f.session(id).lastError, QString("engine offline"));
        QVERIFY(h.f.takeNow(id, &err));
        QCOMPARE(h.f.session(id).status, QString("taking"));
        reply(true, QString());
        QCOMPARE(h.f.session(id).status, QString("live"));
        h.f.noteTakeFailure(id, "timed out waiting for first frame");
        QCOMPARE(h.f.session(id).status, QString("idle"));
        QCOMPARE(h.f.session(id).lastError, QString("timed out waiting for first frame"));
        QVERIFY(h.f.end(id, &err));
    }

    void goNeedsChannel() {
        Harness h;
        const QString id = h.create("Jane", "", 3).value("id").toString();
        QString err; QVERIFY(!h.f.go(id, -1, &err)); QVERIFY(err.contains("channel"));
        QVERIFY(!h.f.go("nope", -1, &err)); QCOMPARE(err, QString("no such session"));
    }

    void endReleasesAndCleansUp() {
        Harness h;
        const QString id = h.create("Jane", "main", 0).value("id").toString();
        QString err; QVERIFY(h.f.go(id, -1, &err)); QCOMPARE(h.f.session(id).status, QString("live"));
        QVERIFY(h.f.end(id, &err));
        const Session s = h.f.session(id);
        QCOMPARE(s.status, QString("ended")); QCOMPARE(s.endedMs, h.now); QVERIFY(!s.feedUp);
        QCOMPARE(h.releases, QStringList{id}); QCOMPARE(h.ingestStopped, QStringList{id}); QCOMPARE(h.inputsRemoved, QStringList{id});
        QVERIFY(h.f.end(id, &err));                    // idempotent
        QCOMPARE(h.releases.size(), 1);
        QVERIFY(!h.f.go(id, -1, &err)); QVERIFY(err.contains("ended"));
        // ending from idle releases nothing (it was never on air)
        const QString id2 = h.create("Jim").value("id").toString();
        QVERIFY(h.f.end(id2, &err)); QCOMPARE(h.releases.size(), 1); QCOMPARE(h.ingestStopped.size(), 2);
        QVERIFY(h.f.remove(id2)); QVERIFY(!h.f.exists(id2)); QVERIFY(!h.f.remove(id2));
        bool found = true; h.f.session(id2, &found); QVERIFY(!found);
    }

    void feedStateAndRestarts() {
        Harness h;
        const QString id = h.create("Jane").value("id").toString();
        h.events.clear();
        h.f.noteFeed(id, true);
        QVERIFY(h.f.session(id).feedUp); QCOMPARE(h.events, QStringList{"feedUp:0"});
        h.f.noteFeed(id, true); QCOMPARE(h.events.size(), 1);       // no change, no event
        h.f.noteFeed(id, false, "publisher left");
        QVERIFY(!h.f.session(id).feedUp); QCOMPARE(h.f.session(id).ingestRestarts, 1); QCOMPARE(h.f.session(id).lastError, QString("publisher left"));
        h.f.noteFeed("nope", true);                                   // unknown id: ignored
    }

    void ffmpegArgsAndMediamtxUrls() {
        Harness h;
        const QJsonObject o = h.create("Jane");
        const Session s = h.f.session(o.value("id").toString());
        const QString key = o.value("streamKey").toString();
        const QStringList a = h.f.ffmpegArgs(s, key);
        QVERIFY(a.contains("-listen")); QCOMPARE(a[a.indexOf("-listen") + 1], QString("1"));
        QCOMPARE(a[a.indexOf("-i") + 1], QString("rtmp://0.0.0.0:1936/reporter/") + key);
        QCOMPARE(a.last(), QString("udp://127.0.0.1:50040?pkt_size=1316"));
        QVERIFY(a.contains("mpegts")); QCOMPARE(a[a.indexOf("-c") + 1], QString("copy"));
        // the monitors' own copy (a UDP port takes one reader: GO opens the engine port while the wall watches this one)
        QVERIFY(a.contains("udp://127.0.0.1:50140?pkt_size=1316")); QVERIFY(a.indexOf("udp://127.0.0.1:50140?pkt_size=1316") < a.indexOf("udp://127.0.0.1:50040?pkt_size=1316"));
        QCOMPARE(h.f.monitorPath(s), QString("udp://127.0.0.1:50140")); QCOMPARE(h.f.inputJson(s).value("monitorUrl").toString(), QString("udp://127.0.0.1:50140"));
        // mediamtx mode: URLs only
        Config c = Config::fromJson(QJsonObject{{"mode", "mediamtx"}, {"mediamtxPublishBase", "rtmp://10.0.0.5:1935"}, {"mediamtxWhipBase", "http://10.0.0.5:8889"}, {"app", "field"}});
        h.f.config = c;
        QCOMPARE(h.f.publishUrl(s, "k1"), QString("rtmp://10.0.0.5:1935/field/k1"));
        QCOMPARE(h.f.whipUrl(s, "k1"), QString("http://10.0.0.5:8889/field/k1/whip"));
        QCOMPARE(h.f.enginePath(s), "rtsp://127.0.0.1:8554/field/" + s.id);
        QVERIFY(h.f.monitorPath(s).isEmpty()); QVERIFY(!h.f.inputJson(s).contains("monitorUrl"));          // RTSP serves many readers
        QCOMPARE(h.f.whipUrl(s, "k1").isEmpty(), false);
        h.f.config.mode = "ffmpeg"; QVERIFY(h.f.whipUrl(s, "k1").isEmpty());
    }

    void configRoundTripAndBounds() {
        Config c = Config::fromJson(QJsonObject{{"rtmpPortBase", 80}, {"portCount", 999}, {"mode", "weird"}, {"app", "bad app!"}, {"defaultCountdownSec", -4}});
        QCOMPARE(c.rtmpPortBase, 1024); QCOMPARE(c.portCount, 64); QCOMPARE(c.mode, QString("ffmpeg")); QCOMPARE(c.app, QString("reporter")); QCOMPARE(c.defaultCountdownSec, 0);
        c.host = "10.1.1.1"; c.udpPortBase = 51000;
        const Config d = Config::fromJson(c.toJson());
        QCOMPARE(d.host, c.host); QCOMPARE(d.udpPortBase, 51000); QCOMPARE(d.mediamtxPullBase, QString("rtsp://127.0.0.1:8554"));
        // a partial update keeps the base
        const Config e = Config::fromJson(QJsonObject{{"host", "10.2.2.2"}}, d);
        QCOMPARE(e.host, QString("10.2.2.2")); QCOMPARE(e.udpPortBase, 51000);
    }

    void keysAreRandomAndHashed() {
        const QString a = ReporterFeed::newKey(), b = ReporterFeed::newKey();
        QCOMPARE(a.size(), 24); QVERIFY(a != b);
        QCOMPARE(ReporterFeed::hashKey("abc"), QString("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
};

QTEST_GUILESS_MAIN(TestReporterFeed)
#include "test_reporterfeed.moc"
