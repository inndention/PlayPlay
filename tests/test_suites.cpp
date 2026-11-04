// Unit tests for the suite rules (playout/supervisor/suitelogic.*): render node choice, split plans, ffmpeg progress,
// the station allowlist, publishing classification and ladder, social blurbs, guided-spot timing and drafts.
#include <QtTest/QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSet>
#include "playout/supervisor/suitelogic.h"

using namespace suites;

class TestSuites : public QObject {
    Q_OBJECT
private:
    static QJsonObject node(const QString& id, const QString& kind, bool ok, int running, int queued, QJsonObject extra = {}) {
        QJsonObject h; h["ok"] = ok; h["ready"] = ok; h["meshRender"] = true; QJsonObject q; q["running"] = running; q["queued"] = queued; q["max"] = 20; h["queue"] = q;
        QJsonObject n; n["id"] = id; n["name"] = id; n["kind"] = kind; n["enabled"] = true; n["render"] = true; n["health"] = h;
        for (auto it = extra.begin(); it != extra.end(); ++it) n[it.key()] = it.value();
        return n;
    }
private slots:
    void chooseLeastBusyEligible() {
        QJsonArray nodes;
        nodes.append(node("b", "playplay", true, 1, 0));
        nodes.append(node("a", "playplay", true, 1, 0));
        nodes.append(node("c", "playplay", true, 0, 0, QJsonObject{{"primary", true}}));      // primary never renders
        nodes.append(node("d", "playplay", false, 0, 0));                                     // down
        nodes.append(node("e", "playplay", true, 2, 0));                                      // too busy (maxBusy 1)
        QCOMPARE(chooseRenderNode(nodes, "playplay").value("id").toString(), QString("a"));   // tie on busy -> name
        QJsonObject notes = node("f", "playplay", true, 0, 0); notes.remove("render"); notes["notes"] = "studio box, render = true";
        QVERIFY(nodeEligible(notes));                                                          // PlayPlay OBS notes convention
        QJsonObject optedOut = node("g", "playplay", true, 0, 0, QJsonObject{{"render", false}});
        QString why; QVERIFY(!nodeEligible(optedOut, &why)); QVERIFY(why.contains("opted"));
        QJsonObject paused = node("h", "playplay", true, 0, 0); QJsonObject h = paused.value("health").toObject(); h["paused"] = true; paused["health"] = h;
        QVERIFY(!nodeEligible(paused));
        QJsonObject old = node("i", "playplay", true, 0, 0); h = old.value("health").toObject(); h["meshRender"] = false; old["health"] = h;
        QVERIFY(!nodeEligible(old));                                                           // no durable request ids -> not safe
        QJsonObject sn = node("s", "spotnode", true, 5, 3); QVERIFY(nodeEligible(sn));       // spot-nodes queue themselves
        QCOMPARE(chooseRenderNode(QJsonArray{sn}, "playplay").isEmpty(), true);
    }
    void splitPlanCoversDuration() {
        const QJsonArray p = planSegments(100.0, 3, {"local", "n1"});
        QCOMPARE(p.size(), 3);
        double sum = 0; for (const QJsonValue& v : p) sum += v.toObject().value("durSec").toDouble();
        QVERIFY(qAbs(sum - 100.0) < 0.01);
        QCOMPARE(p[0].toObject().value("worker").toString(), QString("local"));
        QCOMPARE(p[1].toObject().value("worker").toString(), QString("n1"));
        QCOMPARE(p[2].toObject().value("worker").toString(), QString("local"));
        QCOMPARE(p[2].toObject().value("startSec").toDouble(), 66.667);
        QVERIFY(planSegments(0, 3, {}).isEmpty());
    }
    void presetsAndProgress() {
        bool ok = false; QVERIFY(presetArgs("h264_1080p30", &ok).contains("libx264")); QVERIFY(ok);
        presetArgs("nope", &ok); QVERIFY(!ok);
        QCOMPARE(presetExt("audio_wav"), QString("wav"));
        QCOMPARE(progressSeconds("frame=10\nout_time_us=2500000\nprogress=continue\n"), 2.5);
        QCOMPARE(progressSeconds("out_time=00:01:02.500000\n"), 62.5);
        QCOMPARE(progressSeconds("frame=1\n"), -1.0);
    }
    void stationAllowlist() {
        QVERIFY(stationPathAllowed("GET", "/api/channels"));
        QVERIFY(stationPathAllowed("PUT", "/api/schedule/CH1/3/14"));
        QVERIFY(stationPathAllowed("get", "/api/media?limit=5"));
        QVERIFY(!stationPathAllowed("DELETE", "/api/schedule/CH1/3/14"));
        QVERIFY(!stationPathAllowed("POST", "/api/obs/scene"));
        QVERIFY(!stationPathAllowed("GET", "/api/media/ingest"));
        QVERIFY(!stationPathAllowed("POST", "/api/settings"));
        QVERIFY(!stationPathAllowed("GET", "/api/../api/settings"));
        QVERIFY(fleetCommandAllowed("on_air")); QVERIFY(!fleetCommandAllowed("exec"));
        QVERIFY(starNodeActionAllowed("fleet.query")); QVERIFY(!starNodeActionAllowed("shell"));
    }
    void draftClassification() {
        QJsonObject it; it["source"] = "manual"; it["title"] = "Fair opens Saturday"; it["body"] = "Rides and food all weekend.";
        QCOMPARE(classifyDraft(it, "review").value("status").toString(), QString("pending_approval"));
        it["source"] = "spot_job";
        QCOMPARE(classifyDraft(it, "auto").value("status").toString(), QString("approved"));
        it["body"] = "A man was killed in a crash on Route 58.";
        const QJsonObject held = classifyDraft(it, "auto");
        QCOMPARE(held.value("status").toString(), QString("held")); QCOMPARE(held.value("tier").toString(), QString("absolute"));
        QJsonObject ag; ag["source"] = "agent_draft"; ag["title"] = "Road work"; ag["body"] = "Lane closures next week.";
        QCOMPARE(classifyDraft(ag, "auto").value("status").toString(), QString("held"));          // no source link
        ag["sourceLinks"] = QJsonArray{"http://10.0.0.5/x", "https://192.168.1.2/y"};
        QCOMPARE(classifyDraft(ag, "auto").value("status").toString(), QString("held"));          // private / not https
        ag["sourceLinks"] = QJsonArray{"https://www.virginiadot.org/news"};
        const QJsonObject ok = classifyDraft(ag, "auto");
        QCOMPARE(ok.value("status").toString(), QString("pending_approval")); QVERIFY(ok.value("aiText").toBool());
        QVERIFY(!publicHttpsUrl("https://100.64.0.1/x")); QVERIFY(!publicHttpsUrl("https://localhost/")); QVERIFY(publicHttpsUrl("https://getstarnews.com/a"));
    }
    void ladderAndLabels() {
        QVERIFY(canTransition("pending_approval", "approved"));
        QVERIFY(canTransition("published", "corrected"));
        QVERIFY(!canTransition("published", "draft"));
        QVERIFY(!canTransition("retracted", "approved"));
        QVERIFY(statusInGroup("held", "needs_attention")); QVERIFY(!statusInGroup("published", "open"));
        QJsonObject ai; ai["voice"] = true;
        QCOMPARE(withAiLabel("Hello", ai, "AI label."), QString("Hello\n\nAI label."));
        QCOMPARE(withAiLabel("Hello", QJsonObject(), "AI label."), QString("Hello"));
        QCOMPARE(withAiLabel("Hello\n\nAI label.", ai, "AI label."), QString("Hello\n\nAI label."));
        const QString b = socialBlurb("Rain tonight", "Rain moves in after dark. Expect an inch by morning. Clearing Friday.", "Watch StarNews");
        QVERIFY(b.startsWith("Rain tonight\n\nRain moves in after dark.")); QVERIFY(b.endsWith("Watch StarNews"));
        QCOMPARE(socialBlurb("Same.", "Same.", ""), QString("Same."));
    }
    void sceneTiming() {
        QCOMPARE(sceneCountFor(6), 2); QCOMPARE(sceneCountFor(15), 3); QCOMPARE(sceneCountFor(30), 5); QCOMPARE(sceneCountFor(60), 8);
        QJsonArray s; for (double d : {10.0, 1.0, 1.0, 30.0}) { QJsonObject o; o["durationSec"] = d; s.append(o); }
        for (double len : {6.0, 15.0, 30.0}) {
            const QJsonArray b = balanceScenes(s, len); double sum = 0;
            for (const QJsonValue& v : b) { const double d = v.toObject().value("durationSec").toDouble(); QVERIFY2(d >= 0.79, qPrintable(QString::number(d))); sum += d; }
            QVERIFY2(qAbs(sum - len) < 0.051, qPrintable(QString::number(sum)));
            QCOMPARE(b[3].toObject().value("n").toInt(), 4);
        }
    }
    void draftScriptFromBrief() {
        QJsonObject o; o["brief"] = "Main Street Hardware has 20 percent off all paint this weekend. Free color matching. Open until eight on Saturday.";
        o["lengthSec"] = 15; o["brand"] = QJsonObject{{"name", "Main Street Hardware"}, {"cta", "Visit us today"}};
        const QJsonObject d = draftScript(o, 2);
        const QJsonArray sc = d.value("scenes").toArray();
        QCOMPARE(sc.size(), 3);
        QVERIFY(sc.last().toObject().value("endCard").toBool());
        QVERIFY(sc.first().toObject().value("vo").toString().startsWith("Main Street Hardware."));     // concept 2 opens on the name
        double sum = 0; for (const QJsonValue& v : sc) sum += v.toObject().value("durationSec").toDouble(); QVERIFY(qAbs(sum - 15) < 0.051);
        QCOMPARE(d.value("endCard").toObject().value("vo").toString(), QString("Main Street Hardware. Visit us today."));
        bool askedUrl = false; for (const QJsonValue& q : d.value("questions").toArray()) if (q.toObject().value("id").toString() == "brand.url") askedUrl = true;
        QVERIFY(askedUrl);
        const QString narr = narrationOf(sc, d.value("endCard").toObject());
        QVERIFY(narr.contains("20 percent off")); QVERIFY(narr.endsWith("Visit us today."));         // facts carried verbatim, nothing invented
    }
    void aiModels() {
        QCOMPARE(defaultAiModel(), QString("anthropic/claude-sonnet-5.5"));
        for (const char* id : {"openai/gpt-4o-mini", "openai/gpt-4o-2024-11-20", "openai/gpt-4-turbo", "openai/gpt-4.1", "openai/gpt-3.5-turbo", "openai/chatgpt-4o-latest",
                               "anthropic/claude-3.5-sonnet", "anthropic/claude-3-haiku", "anthropic/claude-2.1", "google/gemini-1.5-pro", "google/gemini-2.0-flash-001", "google/gemini-pro", "gpt-4o-mini"})
            QVERIFY2(isLegacyModel(id), id);
        for (const char* id : {"anthropic/claude-sonnet-5.5", "openai/gpt-6-luna", "google/gemini-3.8-flash", "x-ai/grok-4.7", "openai/gpt-oss-120b"}) QVERIFY2(!isLegacyModel(id), id);
        QCOMPARE(resolveAiModel("openai/gpt-4o-mini"), defaultAiModel());
        QCOMPARE(resolveAiModel(""), defaultAiModel());
        QCOMPARE(resolveAiModel("openai/gpt-6-luna"), QString("openai/gpt-6-luna"));
        auto m = [](const char* id, qint64 created, const char* out = "text") {
            QJsonObject o; o["id"] = id; o["name"] = id; o["created"] = double(created); o["context_length"] = 1000;
            o["pricing"] = QJsonObject{{"prompt", "0.000002"}, {"completion", "0.00001"}};
            o["architecture"] = QJsonObject{{"input_modalities", QJsonArray{"text", "image"}}, {"output_modalities", QJsonArray{out}}}; return o; };
        const qint64 now = 1792700000;
        const QJsonArray data{m("a/old", now - 400 * 86400), m("a/new", now - 10 * 86400), m("a/newer", now - 1 * 86400), m("openai/gpt-4o", now - 5 * 86400), m("a/image-gen", now, "image")};
        const QJsonArray recent = filterModelCatalog(data, now, 365);
        QCOMPARE(recent.size(), 2);
        QCOMPARE(recent[0].toObject().value("id").toString(), QString("a/newer"));       // newest first
        QCOMPARE(recent[0].toObject().value("promptPerMtok").toDouble(), 2.0);
        QCOMPARE(recent[0].toObject().value("completionPerMtok").toDouble(), 10.0);
        QVERIFY(recent[0].toObject().value("vision").toBool());
        const QJsonArray all = filterModelCatalog(data, now, 0);
        QCOMPARE(all.size(), 3);                                                          // age limit off; gpt-4o and image-only still out
        const QJsonArray builtin = builtinModelCatalog();
        QVERIFY(builtin.size() >= 10);
        bool hasDefault = false; for (const QJsonValue& v : builtin) { QVERIFY(!isLegacyModel(v.toObject().value("id").toString())); if (v.toObject().value("id").toString() == defaultAiModel()) hasDefault = true; }
        QVERIFY(hasDefault);
    }
    void operatorTable() {
        QVERIFY(operatorCommandIsAction("skip_current")); QVERIFY(!operatorCommandIsAction("on_air")); QVERIFY(!operatorCommandIsAction("nope"));
        for (const QJsonValue& v : operatorCommands()) QVERIFY(v.toObject().value("params").isObject());
        // the whole-system commands: reads are reads, anything that changes the station is an action (and so needs agents.config.allowActions)
        for (const char* c : {"capabilities", "api_get", "outputs", "facebook_status", "pad_list", "system_stats", "readiness", "flight_plan"}) QVERIFY2(!operatorCommandIsAction(c), c);
        for (const char* c : {"api_call", "output_control", "facebook_go_live", "facebook_end", "pad_press", "spot_generate"}) QVERIFY2(operatorCommandIsAction(c), c);
        QSet<QString> names; for (const QJsonValue& v : operatorCommands()) { const QString n = v.toObject().value("name").toString(); QVERIFY2(!names.contains(n), qPrintable(n)); names.insert(n); }
        QVERIFY(fleetCommandAllowed("outputs")); QVERIFY(fleetCommandAllowed("facebook_status")); QVERIFY(!fleetCommandAllowed("facebook_go_live")); QVERIFY(!fleetCommandAllowed("api_call"));
    }
    void agentApiAccess() {
        QString why;
        QVERIFY(agentApiAllowed("GET", "/api/v1/outputs")); QVERIFY(agentApiAllowed("GET", "/live/facebook")); QVERIFY(agentApiAllowed("get", "/flightplan/main?x=1"));
        QVERIFY(!agentApiAllowed("GET", "/settings/spots.llm", &why)); QVERIFY(!why.isEmpty());
        QVERIFY(!agentApiAllowed("GET", "/api/v1/remote/nodes")); QVERIFY(!agentApiAllowed("GET", "/sessions/export/20261025000000-abcd")); QVERIFY(!agentApiAllowed("GET", "/sessions/20261025000000-abcd/export"));
        QVERIFY(!agentApiAllowed("GET", "/agents/config")); QVERIFY(!agentApiAllowed("GET", "/status/../settings/x")); QVERIFY(!agentApiAllowed("GET", "/status/%2e%2e/settings"));
        QVERIFY(agentApiAllowed("POST", "/channels/main/transport")); QVERIFY(agentApiAllowed("POST", "/live/facebook/start")); QVERIFY(agentApiAllowed("PUT", "/overlays/presets"));
        QVERIFY(agentApiAllowed("POST", "/remote/pad/b1/press")); QVERIFY(!agentApiAllowed("PUT", "/remote/pad")); QVERIFY(!agentApiAllowed("POST", "/remote/pad/b1/delete"));
        QVERIFY(!agentApiAllowed("PUT", "/channels/main", &why)); QVERIFY(why.contains("engine"));
        QVERIFY(!agentApiAllowed("POST", "/settings/x")); QVERIFY(!agentApiAllowed("POST", "/failover/promote")); QVERIFY(!agentApiAllowed("POST", "/sessions/s1/apply"));
        QVERIFY(!agentApiAllowed("POST", "/starnodes/n1/action")); QVERIFY(!agentApiAllowed("POST", "/agents/tasks")); QVERIFY(!agentApiAllowed("POST", "/remote/nodes/n1/proxy"));
        QVERIFY(!agentApiAllowed("DELETE", "/overlays/main/3")); QVERIFY(!agentApiAllowed("PATCH", "/status")); QVERIFY(!agentApiAllowed("POST", "/random/thing"));
        QCOMPARE(routeGroup("/api/v1/live/facebook"), QString("live")); QCOMPARE(routeGroup("/api/channels"), QString("station")); QCOMPARE(routeGroup("/"), QString("root"));
    }
    void redaction() {
        QJsonObject in; in["apiKey"] = "sk-123"; in["streamKey"] = "FB-1234567890"; in["hasKey"] = true; in["hasToken"] = "yes"; in["keyHint"] = "…7890"; in["name"] = "main"; in["token"] = "";
        QJsonObject nest; nest["authToken"] = "abc"; nest["port"] = 7700; nest["url"] = "rtmps://live-api-s.facebook.com:443/rtmp/FB-1234567890-abcdef"; nest["web"] = "http://h/x?token=SECRET&a=1";
        in["inner"] = nest; QJsonArray arr; QJsonObject e; e["password"] = "pw"; e["label"] = "x"; arr.append(e); in["list"] = arr;
        const QJsonObject o = redactSecrets(in).toObject();
        QCOMPARE(o["apiKey"].toString(), QString("(hidden)")); QCOMPARE(o["streamKey"].toString(), QString("(hidden)")); QCOMPARE(o["hasKey"].toBool(), true); QCOMPARE(o["hasToken"].toString(), QString("yes"));
        QCOMPARE(o["keyHint"].toString(), in["keyHint"].toString()); QCOMPARE(o["name"].toString(), QString("main")); QCOMPARE(o["token"].toString(), QString(""));
        const QJsonObject i = o["inner"].toObject(); QCOMPARE(i["authToken"].toString(), QString("(hidden)")); QCOMPARE(i["port"].toInt(), 7700);
        QVERIFY(!i["url"].toString().contains("FB-1234567890")); QVERIFY(i["url"].toString().startsWith("rtmps://live-api-s.facebook.com:443/rtmp/"));
        QVERIFY(!i["web"].toString().contains("SECRET")); QVERIFY(i["web"].toString().contains("a=1"));
        QCOMPARE(o["list"].toArray()[0].toObject()["password"].toString(), QString("(hidden)")); QCOMPARE(o["list"].toArray()[0].toObject()["label"].toString(), QString("x"));
        const QString dump = QString::fromUtf8(QJsonDocument(o).toJson()); QVERIFY(!dump.contains("sk-123")); QVERIFY(!dump.contains("abcdef"));
    }
    void hubMayReadTheWholeSystem() {
        for (const char* p : {"/api/v1/outputs", "/api/v1/readiness", "/api/v1/system/stats", "/api/v1/live/facebook", "/api/v1/remote/pad", "/api/v1/channels", "/api/v1/channels/main/outputs", "/api/v1/flightplan/main"}) QVERIFY2(stationPathAllowed("GET", p), p);
        QVERIFY(stationPathAllowed("POST", "/api/v1/channels/main/transport")); QVERIFY(stationPathAllowed("POST", "/api/v1/remote/pad/b1/press"));
        QVERIFY(!stationPathAllowed("POST", "/api/v1/live/facebook/start")); QVERIFY(!stationPathAllowed("PUT", "/api/v1/live/facebook")); QVERIFY(!stationPathAllowed("GET", "/api/v1/settings/rest.token"));
        QVERIFY(!stationPathAllowed("PUT", "/api/v1/remote/pad")); QVERIFY(!stationPathAllowed("GET", "/api/v1/remote/nodes"));
    }
};

QTEST_GUILESS_MAIN(TestSuites)
#include "test_suites.moc"
