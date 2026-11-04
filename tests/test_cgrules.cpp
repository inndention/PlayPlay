// CG rules (inputs, any/all, negate), pillar panels decisions and parsers, SCTE-35 helpers.
// docs/overlays.md "Rules", docs/pillar-panels.md, docs/mux.md "SCTE-35".
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "../playout/supervisor/overlaymanager.h"
#include "../playout/supervisor/pillarpanels.h"
#include "../playout/supervisor/scte35.h"

using J = QJsonObject;

static J obj(const char* json) { return QJsonDocument::fromJson(QByteArray(json)).object(); }
// JSON written with single quotes (moc misreads some raw string literals)
static J sq(const char* json) { return QJsonDocument::fromJson(QByteArray(json).replace('\'', '"')).object(); }

class TestCgRules : public QObject {
    Q_OBJECT
private slots:
    // ------------------------------------------------------------------ rules
    void adsSuppressByType() {
        const J r = obj(R"({"when":{"types":["ad","promo"]},"then":{"suppress":["*"]}})");
        J why;
        QVERIFY(OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"ad"})")), &why));
        QVERIFY(!OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"program","mediaType":"news"})")), &why));
        // the library type counts too: a promo scheduled as "program"
        QVERIFY(OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"program","mediaType":"promo"})")), &why));
    }
    void programAndBreakRules() {
        const J allow = obj(R"({"negate":true,"when":{"mode":"any","types":["live","program","news","weather"],"categories":["live","program","news","weather"]}})");
        const J breaks = obj(R"({"when":{"mode":"any","types":["ad","ads","promo","promos","id","ids","station_id"],"categories":["ad","ads","promo","promos","id","ids","station_id"]}})");
        auto match = [](const J& r, const char* json) { return OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(json)), nullptr); };
        QVERIFY(!match(allow, R"({"type":"program","mediaType":"weather"})"));
        QVERIFY(!match(allow, R"({"type":"live","live":true})"));
        QVERIFY(match(breaks, R"({"type":"program","mediaType":"promo"})"));
        QVERIFY(match(breaks, R"({"type":"program","category":"ID"})"));
        QVERIFY(!match(breaks, R"({"type":"program","mediaType":"news"})"));
    }
    void onlyOnNewsOrLive() {
        // "only draw on NEWS or LIVE" = suppress everything when the item is neither
        const J r = obj(R"({"negate":true,"when":{"mode":"any","types":["news","live"],"categories":["news"]},"then":{"suppress":["*"]}})");
        J why;
        auto fires = [&](const char* m) { return OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(m)), &why); };
        QVERIFY(fires(R"({"type":"ad"})"));                                   // an ad: graphics off
        QVERIFY(fires(R"({"type":"program","mediaType":"program"})"));        // a movie: off
        QVERIFY(!fires(R"({"type":"program","mediaType":"news"})"));          // news: graphics come back
        QVERIFY(!fires(R"({"type":"program","category":"News"})"));           // category News
        QVERIFY(!fires(R"({"type":"live","live":true})"));                   // a live take
        QCOMPARE(why.value("negate").toBool(), true);
        // negating "every item" (no criteria) never fires
        QVERIFY(!OverlayManager::ruleMatches(obj(R"({"negate":true,"when":{}})"), "main", OverlayManager::testMedia(obj(R"({"type":"ad"})")), &why));
    }
    void allModeNeedsEveryCriterion() {
        const J r = obj(R"({"when":{"types":["news"],"keywords":["weather"]}})");
        J why;
        QVERIFY(OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"news","text":"the weather at six"})")), &why));
        QVERIFY(!OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"news","text":"city council"})")), &why));
        QCOMPARE(why.value("keywords").toBool(), false);
    }
    void inputsBySlotPathAndTitle() {
        const auto cam = OverlayManager::testMedia(obj("{\"type\":\"live\",\"live\":true,\"path\":\"ndi://CAMERA (NDIHX2-192.168.1.64)\",\"input\":\"in2\",\"inputTitle\":\"Anchor desk\"}"));
        J why;
        QVERIFY(OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["in2"]}})"), "main", cam, &why));
        QVERIFY(OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["2"]}})"), "main", cam, &why));
        QVERIFY(!OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["in3"]}})"), "main", cam, &why));
        QVERIFY(!OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["12"]}})"), "main", cam, &why));     // slot 12 is not slot 2
        QVERIFY(OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["NDIHX2-192.168.1.64"]}})"), "main", cam, &why));
        QVERIFY(OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["anchor"]}})"), "main", cam, &why));
        // a file is not an input
        QVERIFY(!OverlayManager::ruleMatches(obj(R"({"when":{"inputs":["in2"]}})"), "main", OverlayManager::testMedia(obj(R"({"type":"program","path":"D:/media/show.mp4"})")), &why));
    }
    // ------------------------------------------------------------------ CG slots (CG1, CG2 ... on switcher inputs)
    void cgSlotNormalize() {
        const J s = OverlayManager::normalizeCgSlot(sq("{'template':'lower_third_modern','params':{'text1':'Jane'},'inputs':['3','Input 5','in3']}"), 1);
        QCOMPARE(s.value("id").toString(), QString("cg2"));
        QCOMPARE(s.value("name").toString(), QString("CG2"));
        QCOMPARE(s.value("inputs").toArray(), (QJsonArray{"in3", "in5"}));
        QCOMPARE(s.value("startOffset").toDouble(), 1.0);
        QCOMPARE(s.value("duration").toDouble(), 0.0);
        // "all" wins over a list
        QCOMPARE(OverlayManager::normalizeCgSlot(sq("{'inputs':['in1','all']}"), 0).value("inputs").toArray(), (QJsonArray{"*"}));
        QCOMPARE(OverlayManager::normCgInput("ANY"), QString("*"));
        QCOMPARE(OverlayManager::normCgInput("0"), QString());
    }
    void cgSlotRuleFiresOnItsInputs() {
        const auto in2 = OverlayManager::testMedia(sq("{'type':'live','live':true,'path':'ndi://CAM2','input':'in2'}"));
        const auto in4 = OverlayManager::testMedia(sq("{'type':'live','live':true,'path':'ndi://CAM4','input':'in4'}"));
        const auto film = OverlayManager::testMedia(sq("{'type':'program','path':'D:/media/show.mp4'}"));
        J why;
        const J some = OverlayManager::cgSlotRule(OverlayManager::normalizeCgSlot(sq("{'id':'cg1','template':'lower_third_modern','inputs':['in2']}"), 0));
        QCOMPARE(some.value("id").toString(), QString("cg:cg1"));
        QCOMPARE(some.value("then").toObject().value("show").toObject().value("label").toString(), QString("CG1"));
        QVERIFY(OverlayManager::ruleMatches(some, "main", in2, &why));
        QVERIFY(!OverlayManager::ruleMatches(some, "main", in4, &why));
        const J all = OverlayManager::cgSlotRule(OverlayManager::normalizeCgSlot(sq("{'id':'cg2','template':'bug_live_indicator','inputs':['*']}"), 1));
        QVERIFY(OverlayManager::ruleMatches(all, "main", in2, &why));
        QVERIFY(OverlayManager::ruleMatches(all, "main", in4, &why));
        QVERIFY(!OverlayManager::ruleMatches(all, "main", film, &why));      // "all inputs" = every bus input, not every item
        // no graphic, no inputs or disabled: no rule
        QVERIFY(OverlayManager::cgSlotRule(OverlayManager::normalizeCgSlot(sq("{'inputs':['*']}"), 0)).isEmpty());
        QVERIFY(OverlayManager::cgSlotRule(OverlayManager::normalizeCgSlot(sq("{'template':'x'}"), 0)).isEmpty());
        QVERIFY(OverlayManager::cgSlotRule(OverlayManager::normalizeCgSlot(sq("{'template':'x','inputs':['*'],'enabled':false}"), 0)).isEmpty());
    }
    void cgAssignFromTheInput() {
        QJsonArray cgs{ OverlayManager::normalizeCgSlot(sq("{'id':'cg1','inputs':['*']}"), 0),
                          OverlayManager::normalizeCgSlot(sq("{'id':'cg2','inputs':['in1']}"), 1),
                          OverlayManager::normalizeCgSlot(sq("{'id':'cg3'}"), 2) };
        // input 3: CG3 only -> CG1 ("all") becomes every other input, CG3 gets in3, CG2 untouched
        cgs = OverlayManager::assignCgInputIn(cgs, "3", {"cg3"}, 4);
        QCOMPARE(cgs[0].toObject().value("inputs").toArray(), (QJsonArray{"in1", "in2", "in4"}));
        QCOMPARE(cgs[1].toObject().value("inputs").toArray(), (QJsonArray{"in1"}));
        QCOMPARE(cgs[2].toObject().value("inputs").toArray(), (QJsonArray{"in3"}));
        // input 1: nothing
        cgs = OverlayManager::assignCgInputIn(cgs, "in1", {}, 4);
        QVERIFY(!OverlayManager::cgCoversInput(cgs[0].toObject(), "in1"));
        QVERIFY(cgs[1].toObject().value("inputs").toArray().isEmpty());
        QVERIFY(OverlayManager::cgCoversInput(cgs[0].toObject(), "in2"));
    }
    void channelsAlwaysScope() {
        const J r = obj(R"({"channels":["ch2"],"negate":true,"when":{"types":["news"]}})");
        J why;
        QVERIFY(!OverlayManager::ruleMatches(r, "main", OverlayManager::testMedia(obj(R"({"type":"ad"})")), &why));   // other channel: never
        QVERIFY(OverlayManager::ruleMatches(r, "ch2", OverlayManager::testMedia(obj(R"({"type":"ad"})")), &why));
    }

    // ------------------------------------------------------------------ pillar panels
    void aspectFromItem() {
        QCOMPARE(PillarPanels::displayAspect(obj(R"({"width":1080,"height":1920})")), 1080.0 / 1920.0);
        QCOMPARE(PillarPanels::displayAspect(obj(R"({"width":1920,"height":1080,"extra":{"rotation":90}})")), 1080.0 / 1920.0);
        QCOMPARE(PillarPanels::displayAspect(obj(R"({"width":1920,"height":1080,"extra":{"displayAspect":"4:3"}})")), 4.0 / 3.0);
        QCOMPARE(PillarPanels::displayAspect(obj(R"({"width":720,"height":480,"extra":{"sar":"8:9"}})")), 720.0 * 8 / 9 / 480.0);
        QCOMPARE(PillarPanels::displayAspect(obj(R"({"width":0,"height":0})")), 0.0);
    }
    void qualifiesRules() {
        J cfg = PillarPanels::normalize(obj(R"({"enabled":true})"));
        double a = 0; QString why;
        QVERIFY(PillarPanels::qualifies(cfg, "main", obj(R"({"type":"program","width":1080,"height":1920})"), &a, &why));
        QVERIFY(qAbs(a - 0.5625) < 1e-9);
        QVERIFY(PillarPanels::qualifies(cfg, "main", obj(R"({"type":"program","width":1440,"height":1080})"), &a, &why));     // 4:3
        QVERIFY(!PillarPanels::qualifies(cfg, "main", obj(R"({"type":"program","width":1920,"height":1080})"), &a, &why));    // 16:9: no bars
        QVERIFY(!PillarPanels::qualifies(cfg, "main", obj(R"({"type":"live","live":true})"), &a, &why));                      // live: size unknown
        QVERIFY(why.contains("live"));
        QVERIFY(PillarPanels::qualifies(cfg, "main", obj(R"({"type":"live","live":true,"extra":{"displayAspect":"9:16"}})"), &a, &why));
        QVERIFY(PillarPanels::qualifies(cfg, "main", obj(R"({"type":"program","width":1920,"height":1080,"extra":{"pillarPanels":true}})"), &a, &why));   // forced
        QVERIFY(!PillarPanels::qualifies(cfg, "main", obj(R"({"type":"program","width":1080,"height":1920,"extra":{"pillarPanels":false}})"), &a, &why));
        QVERIFY(!PillarPanels::qualifies(PillarPanels::normalize(obj(R"({"enabled":false})")), "main", obj(R"({"width":1080,"height":1920})"), &a, &why));
        QVERIFY(!PillarPanels::qualifies(PillarPanels::normalize(obj(R"({"enabled":true,"channels":["ch2"]})")), "main", obj(R"({"width":1080,"height":1920})"), &a, &why));
        QVERIFY(!PillarPanels::qualifies(PillarPanels::normalize(obj(R"({"enabled":true,"excludeTypes":["ad"]})")), "main", obj(R"({"type":"ad","width":1080,"height":1920})"), &a, &why));
        QVERIFY(!PillarPanels::qualifies(cfg, "main", J(), &a, &why));
        // maxAspect never reaches 16:9
        QVERIFY(PillarPanels::normalize(obj(R"({"maxAspect":3})")).value("maxAspect").toDouble() < 16.0 / 9.0);
    }
    void weatherParse() {
        const J om = obj(R"({"current":{"temperature_2m":63.6,"apparent_temperature":61.2,"weather_code":2,"is_day":1,"wind_speed_10m":7.4},
            "daily":{"time":["2026-10-23","2026-10-24","2026-10-25","2026-10-26"],"weather_code":[2,0,61,3],"temperature_2m_max":[68.2,70.1,63,59],
                     "temperature_2m_min":[47,49,45,41],"precipitation_probability_max":[5,0,80,20]}})");
        const J w = PillarPanels::parseWeather(om, QTimeZone("America/New_York"));
        QCOMPARE(w.value("tempF").toInt(), 64);
        QCOMPARE(w.value("icon").toString(), QString("partly"));
        QCOMPARE(w.value("cond").toString(), QString("Partly Cloudy"));
        QCOMPARE(w.value("hiF").toInt(), 68);
        QCOMPARE(w.value("days").toArray().size(), 3);
        QCOMPARE(w.value("days").toArray()[1].toObject().value("icon").toString(), QString("rain"));
        const J night = PillarPanels::parseWeather(obj(R"({"current":{"temperature_2m":40,"weather_code":0,"is_day":0}})"), QTimeZone::utc());
        QCOMPARE(night.value("icon").toString(), QString("moon"));
        QVERIFY(PillarPanels::parseWeather(J(), QTimeZone::utc()).isEmpty());
    }
    void historyParse() {
        const J wiki = obj(R"({"selected":[{"year":1958,"text":"The Smurfs appear for the first time in a Belgian comics magazine."},
            {"year":2001,"text":"Short."},{"year":1911,"text":"An Italian pilot makes the first military use of an aircraft in Libya."},
            {"year":1864,"text":"Union forces win the Battle of Westport, the largest battle west of the Mississippi River."}]})");
        const QJsonArray h = PillarPanels::parseHistory(wiki, 8);
        QCOMPARE(h.size(), 3);                                       // the 6-character entry is dropped
        QCOMPARE(h[0].toObject().value("year").toInt(), 1958);       // newest first
        // the default avoid list keeps casualty news out of a feel-good panel; "Dieppe" must not trip on "die"
        const J grim = obj(R"({"selected":[{"year":2022,"text":"Military airstrikes killed at least 80 concertgoers in Kachin State."},
            {"year":1942,"text":"Allied forces land at Dieppe in a raid on the coast of occupied France."}]})");
        const QJsonArray g = PillarPanels::parseHistory(grim, 8);
        QCOMPARE(g.size(), 1);
        QCOMPARE(g[0].toObject().value("year").toInt(), 1942);
    }
    void upNextSkipsBreaksAndRepeats() {
        const qint64 now = 1800000000000LL;
        J rl; J cur; QJsonArray items;
        auto add = [&](const char* id, const char* title, const char* type, qint64 startOffMs) { J it; it["id"] = id; it["title"] = title; it["type"] = type; it["plannedStartMs"] = double(now + startOffMs); items.append(it); };
        add("a", "Morning Show", "program", -60000);                 // on air: in the past
        add("b", "Car Lot Ad", "ad", 60000);
        add("c", "Evening News", "program", 120000);
        add("d", "Evening News", "program", 300000);                 // a second segment of the same show
        add("e", "Weather Wrap", "program", 600000);
        cur["items"] = items; rl["current"] = cur;
        const QJsonArray n = PillarPanels::upNextFrom(rl, now, 3, {"ad", "promo"}, QString(), QTimeZone::utc());
        QCOMPARE(n.size(), 2);
        QCOMPARE(n[0].toObject().value("title").toString(), QString("Evening News"));
        QCOMPARE(n[1].toObject().value("title").toString(), QString("Weather Wrap"));
    }

    // ------------------------------------------------------------------ SCTE-35
    void spliceXml() {
        scte35::Splice s; s.eventId = 1001; s.durationSec = 30; s.uniqueProgramId = 101;
        const QString x = scte35::spliceInsertXml(s);
        QVERIFY(x.contains("splice_event_id=\"1001\""));
        QVERIFY(x.contains("out_of_network=\"true\""));
        QVERIFY(x.contains("splice_immediate=\"true\""));
        QVERIFY(x.contains("duration=\"2700000\""));                // 30 s at 90 kHz
        s.outOfNetwork = false; s.durationSec = 0;
        const QString in = scte35::spliceInsertXml(s);
        QVERIFY(in.contains("out_of_network=\"false\""));
        QVERIFY(!in.contains("break_duration"));
        QVERIFY(scte35::spliceCancelXml(7).contains("splice_event_cancel=\"true\""));
        QVERIFY(scte35::spliceNullXml().contains("<splice_null/>"));
    }
    void tspArgsFollowToggles() {
        QJsonArray svcs; svcs.append(obj(R"({"id":"svc1","serviceId":101})")); svcs.append(obj(R"({"id":"svc2","serviceId":102,"scte35":{"enabled":false}})"));
        svcs.append(obj(R"({"id":"svc3","serviceId":103,"scte35":{"pid":"0x1F5"}})"));
        QVERIFY(scte35::tspPluginArgs(scte35::normalize(J()), svcs).isEmpty());                 // master switch off: the mux is unchanged
        const QStringList a = scte35::tspPluginArgs(scte35::normalize(obj(R"({"enabled":true})")), svcs);
        QCOMPARE(a.count("pmt"), 2);                                                            // svc2 is switched off
        QCOMPARE(a.count("spliceinject"), 2);
        QVERIFY(a.contains("0x0400/0x86"));                                                     // svc1 default PID
        QVERIFY(a.contains("0x01f5/0x86"));                                                     // svc3 explicit PID
        QVERIFY(a.contains("127.0.0.1:5310"));
        QVERIFY(a.contains("127.0.0.1:5312"));                                                  // port follows the service index
        QVERIFY(a.contains("0x43554549"));
    }
    void breakLength() {
        J rl; J cur, nxt; QJsonArray a, b;
        auto it = [](const char* id, const char* type, double d) { J o; o["id"] = id; o["type"] = type; o["duration"] = d; return o; };
        a.append(it("p1", "program", 600)); a.append(it("ad1", "ad", 30)); a.append(it("ad2", "ad", 15));
        b.append(it("pr1", "promo", 10)); b.append(it("p2", "program", 900));
        cur["items"] = a; nxt["items"] = b; rl["current"] = cur; rl["next"] = nxt;
        QCOMPARE(scte35::breakSecondsFrom(rl, "ad1", {"ad", "promo"}), 55.0);                   // runs across the hour boundary
        QCOMPARE(scte35::breakSecondsFrom(rl, "nope", {"ad"}), 0.0);
        QVERIFY(scte35::isBreakType(scte35::normalize(J()), "AD"));
        QVERIFY(!scte35::isBreakType(scte35::normalize(J()), "program"));
    }
};

QTEST_GUILESS_MAIN(TestCgRules)
#include "test_cgrules.moc"
