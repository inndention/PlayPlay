// Unit tests for the publishing lane's hard rails (playout/supervisor/pubrails.*, ST-30): every rail as a table test.
// Approval bound to {rev, bodySha256}; per-hour / per-day caps and the minimum gap; outcome classes (published / failed /
// unknown) for Facebook Graph and WordPress answers; backoff; reconcile matching and verdicts; restart recovery; the item
// ladder (aggregate status, actions refused while publishing / reconciling).
#include <QtTest/QtTest>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include "playout/supervisor/pubrails.h"

using namespace pubrails;

static constexpr qint64 kMin = 60000;
static QString utcIso(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString("yyyy-MM-dd'T'HH:mm:ss"); }

class TestPubRails : public QObject {
    Q_OBJECT
private:
    // 18:00 local on a fixed day: room for "earlier today" and "yesterday" offsets
    static qint64 now() { return QDateTime(QDate(2026, 10, 26), QTime(18, 0)).toMSecsSinceEpoch(); }
    static QJsonObject baseItem() {
        QJsonObject it; it["id"] = "pub_1"; it["kind"] = "text"; it["title"] = "Road closed on Main Street"; it["body"] = "Crews are repairing a water line.";
        it["caption"] = "Main Street is closed until 6 pm."; it["excerpt"] = "Main St closed"; it["linkUrl"] = ""; it["canonicalUrl"] = ""; it["mediaId"] = ""; it["category"] = "traffic";
        it["channels"] = QJsonArray{"facebook", "website"}; it["tags"] = QJsonArray{"traffic", "martinsville"}; it["ai"] = QJsonObject{{"text", true}, {"voice", false}};
        it["revision"] = 1; it["status"] = "approved"; it["perChannel"] = QJsonObject();
        return it;
    }
    // one item with a single channel state; minutesAgo is applied to postedMs (published) or attemptStartedMs (in flight)
    static QJsonObject posted(const QString& channel, const QString& state, int minutesAgo) {
        QJsonObject one; one["state"] = state; const double t = double(now() - qint64(minutesAgo) * kMin);
        if (state == "published") one["postedMs"] = t; else one["attemptStartedMs"] = t;
        QJsonObject pc; pc[channel] = one;
        QJsonObject it; it["id"] = QString("p%1").arg(minutesAgo); it["channels"] = QJsonArray{channel}; it["perChannel"] = pc; it["status"] = state == "published" ? "published" : "reconcile";
        return it;
    }
    static QJsonObject withChannels(const QJsonObject& states) {   // {channel: {state, attempts, retryable, lastTryMs}}
        QJsonObject it = baseItem(); QJsonArray ch; for (auto i = states.begin(); i != states.end(); ++i) ch.append(i.key());
        it["channels"] = ch; it["perChannel"] = states; return it;
    }

private slots:
    // ---------------------------------------------------------------- approval bound to {rev, bodySha256}
    void approvalBinding_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<bool>("stale");
        // content: any change makes the approval stale
        QTest::newRow("title") << "title" << QJsonValue("Road open on Main Street") << true;
        QTest::newRow("body") << "body" << QJsonValue("Crews finished early.") << true;
        QTest::newRow("caption") << "caption" << QJsonValue("Main Street is open.") << true;
        QTest::newRow("excerpt") << "excerpt" << QJsonValue("Main St open") << true;
        QTest::newRow("linkUrl") << "linkUrl" << QJsonValue("https://example.org/x") << true;
        QTest::newRow("canonicalUrl") << "canonicalUrl" << QJsonValue("https://example.org/c") << true;
        QTest::newRow("mediaId") << "mediaId" << QJsonValue("m42") << true;
        QTest::newRow("category") << "category" << QJsonValue("weather") << true;
        QTest::newRow("kind") << "kind" << QJsonValue("link") << true;
        QTest::newRow("channels added") << "channels" << QJsonValue(QJsonArray{"facebook"}) << true;
        QTest::newRow("tags") << "tags" << QJsonValue(QJsonArray{"traffic"}) << true;
        QTest::newRow("tag order") << "tags" << QJsonValue(QJsonArray{"martinsville", "traffic"}) << true;
        QTest::newRow("ai label") << "ai" << QJsonValue(QJsonObject{{"text", false}, {"voice", false}}) << true;
        // bookkeeping: never part of the content
        QTest::newRow("status") << "status" << QJsonValue("scheduled") << false;
        QTest::newRow("why") << "why" << QJsonValue("approved by ops") << false;
        QTest::newRow("history") << "history" << QJsonValue(QJsonArray{QJsonObject{{"action", "approve"}}}) << false;
        QTest::newRow("perChannel") << "perChannel" << QJsonValue(QJsonObject{{"facebook", QJsonObject{{"state", "failed"}}}}) << false;
        QTest::newRow("scheduleAtMs") << "scheduleAtMs" << QJsonValue(double(now() + 3600000)) << false;
        QTest::newRow("updatedMs") << "updatedMs" << QJsonValue(double(now())) << false;
        QTest::newRow("channel order") << "channels" << QJsonValue(QJsonArray{"website", "facebook"}) << false;
    }
    void approvalBinding() {
        QFETCH(QString, field); QFETCH(QJsonValue, value); QFETCH(bool, stale);
        QJsonObject it = baseItem();
        it["approval"] = bindApproval(it, "ops", now());
        QVERIFY(approvalProblem(it).isEmpty());
        it[field] = value;
        QCOMPARE(!approvalProblem(it).isEmpty(), stale);
    }
    void approvalRevisionAndMedia() {
        QJsonObject it = baseItem();
        QVERIFY(approvalProblem(it).contains("no approval"));                     // never approved (or approved before the rails)
        it["approvedBy"] = "ops";                                                  // a legacy approval is not a binding
        QVERIFY(!approvalProblem(it).isEmpty());
        it["approval"] = bindApproval(it, "ops", now(), "spot.mp4|1000");
        QCOMPARE(it.value("approval").toObject().value("rev").toInt(), 1);
        QVERIFY(approvalProblem(it, "spot.mp4|1000").isEmpty());
        QVERIFY(approvalProblem(it, "spot.mp4|2000").contains("stale"));         // the file was re-rendered
        QVERIFY(!approvalProblem(it).isEmpty());                                   // or removed
        it["revision"] = 2;                                                        // a revision always needs a new approval, even with equal text
        QVERIFY(approvalProblem(it, "spot.mp4|1000").contains("revision 1"));
    }
    void textHashIsNormalised() {
        QCOMPARE(textSha256("Main Street  is CLOSED\n\nuntil 6 pm. "), textSha256("main street is closed until 6 pm."));
        QVERIFY(textSha256("Main Street is closed") != textSha256("Main Street is open"));
        QCOMPARE(textSha256("x").size(), 64);
    }

    // ---------------------------------------------------------------- limits: code constants, config only tightens
    void limits_data() {
        QTest::addColumn<QString>("channel");
        QTest::addColumn<QJsonObject>("cfg");
        QTest::addColumn<int>("perHour");
        QTest::addColumn<int>("perDay");
        QTest::addColumn<int>("gap");
        QTest::newRow("facebook hard") << "facebook" << QJsonObject() << 4 << 15 << 10;
        QTest::newRow("facebook tighter") << "facebook" << QJsonObject{{"facebook", QJsonObject{{"perHour", 2}, {"perDay", 6}, {"minGapMin", 30}}}} << 2 << 6 << 30;
        QTest::newRow("facebook looser ignored") << "facebook" << QJsonObject{{"facebook", QJsonObject{{"perHour", 50}, {"perDay", 100}, {"minGapMin", 1}}}} << 4 << 15 << 10;
        QTest::newRow("facebook closed") << "facebook" << QJsonObject{{"facebook", QJsonObject{{"perHour", 0}}}} << 0 << 15 << 10;
        QTest::newRow("facebook junk") << "facebook" << QJsonObject{{"facebook", QJsonObject{{"perHour", "9"}, {"minGapMin", -5}}}} << 4 << 15 << 10;
        QTest::newRow("website none") << "website" << QJsonObject() << -1 << -1 << 0;
        QTest::newRow("website configured") << "website" << QJsonObject{{"website", QJsonObject{{"perHour", 6}, {"minGapMin", 5}}}} << 6 << -1 << 5;
    }
    void limits() {
        QFETCH(QString, channel); QFETCH(QJsonObject, cfg); QFETCH(int, perHour); QFETCH(int, perDay); QFETCH(int, gap);
        const Limits l = limitsFor(channel, cfg);
        QCOMPARE(l.perHour, perHour); QCOMPARE(l.perDay, perDay); QCOMPARE(l.minGapMin, gap);
    }

    // ---------------------------------------------------------------- per-hour cap, per-day cap, minimum gap
    void rateRails_data() {
        QTest::addColumn<QJsonArray>("items");
        QTest::addColumn<int>("hour");
        QTest::addColumn<int>("day");
        QTest::addColumn<QString>("failing");          // the first failing rail, empty = may post now
        QTest::addColumn<int>("nextInMin");            // minutes from now until the next post may go (0 = now; -2 = next midnight)
        QTest::newRow("empty") << QJsonArray() << 0 << 0 << "" << 0;
        QTest::newRow("3 in the hour, gap ok") << QJsonArray{posted("facebook", "published", 11), posted("facebook", "published", 25), posted("facebook", "published", 40)} << 3 << 3 << "" << 0;
        QTest::newRow("hourly cap") << QJsonArray{posted("facebook", "published", 11), posted("facebook", "published", 25), posted("facebook", "published", 40), posted("facebook", "published", 52)} << 4 << 4 << "rail:facebook_hour" << 8;
        QTest::newRow("gap") << QJsonArray{posted("facebook", "published", 5)} << 1 << 1 << "rail:facebook_gap" << 5;
        QTest::newRow("older than an hour") << QJsonArray{posted("facebook", "published", 61), posted("facebook", "published", 75), posted("facebook", "published", 90), posted("facebook", "published", 120)} << 0 << 4 << "" << 0;
        QTest::newRow("unknown counts") << QJsonArray{posted("facebook", "unknown", 3)} << 1 << 1 << "rail:facebook_gap" << 7;
        QTest::newRow("publishing counts") << QJsonArray{posted("facebook", "publishing", 1)} << 1 << 1 << "rail:facebook_gap" << 9;
        QTest::newRow("failed does not count") << QJsonArray{posted("facebook", "failed", 1)} << 0 << 0 << "" << 0;
        QTest::newRow("other channel") << QJsonArray{posted("website", "published", 1)} << 0 << 0 << "" << 0;
        QJsonArray day; for (int i = 0; i < 15; ++i) day.append(posted("facebook", "published", 70 + i * 40));   // 15 posts between 07:30 and 16:50
        QTest::newRow("daily cap") << day << 0 << 15 << "rail:facebook_day" << -2;
    }
    void rateRails() {
        QFETCH(QJsonArray, items); QFETCH(int, hour); QFETCH(int, day); QFETCH(QString, failing); QFETCH(int, nextInMin);
        const qint64 n = now(), d0 = localDayStartMs(n);
        const Stats s = statsFor(items, "facebook", n, d0);
        QCOMPARE(s.hour, hour); QCOMPARE(s.day, day);
        const Limits l = limitsFor("facebook", QJsonObject());
        QString first; for (const Check& c : rateChecks("facebook", s, l, n, d0)) if (!c.pass && first.isEmpty()) first = c.name;
        QCOMPARE(first, failing);
        const qint64 next = nextAllowedMs(s, l, n, d0);
        if (nextInMin == -2) QCOMPARE(next, nextDayStartMs(d0));
        else QCOMPARE(next, n + qint64(nextInMin) * kMin);
        QCOMPARE(next <= n, failing.isEmpty());                                    // checks and the next-allowed time agree
    }
    void rateClosedAndWebsite() {
        const qint64 n = now(), d0 = localDayStartMs(n);
        QCOMPARE(nextAllowedMs(Stats(), limitsFor("facebook", QJsonObject{{"facebook", QJsonObject{{"perDay", 0}}}}), n, d0), qint64(-1));
        const Stats s = statsFor(QJsonArray{posted("website", "published", 1), posted("website", "published", 2)}, "website", n, d0);
        QVERIFY(rateChecks("website", s, limitsFor("website", QJsonObject()), n, d0).isEmpty());   // no website rails unless configured
        bool anyFail = false; for (const Check& c : rateChecks("website", s, limitsFor("website", QJsonObject{{"website", QJsonObject{{"perHour", 2}}}}), n, d0)) anyFail |= !c.pass;
        QVERIFY(anyFail);
    }

    // ---------------------------------------------------------------- outcome classes
    void classify_data() {
        QTest::addColumn<QString>("channel");
        QTest::addColumn<int>("http");
        QTest::addColumn<QJsonObject>("json");
        QTest::addColumn<QString>("netError");
        QTest::addColumn<QString>("remoteId");
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("cls");
        QTest::addColumn<bool>("retryable");
        auto graph = [](int code, bool transient = false) { QJsonObject e{{"message", "x"}, {"code", code}}; if (transient) e["is_transient"] = true; return QJsonObject{{"error", e}}; };
        QTest::newRow("fb 200 id") << "facebook" << 200 << QJsonObject{{"id", "1_2"}} << "" << "1_2" << "published" << "" << false;
        QTest::newRow("fb 200 no id") << "facebook" << 200 << QJsonObject() << "" << "" << "unknown" << "bad_answer" << false;
        QTest::newRow("fb timeout") << "facebook" << 0 << QJsonObject() << "timed out" << "" << "unknown" << "no_answer" << false;
        QTest::newRow("fb closed mid-call") << "facebook" << 0 << QJsonObject() << "Remote host closed the connection" << "" << "unknown" << "no_answer" << false;
        QTest::newRow("fb refused") << "facebook" << 0 << QJsonObject() << "Connection refused" << "" << "failed" << "not_sent" << true;
        QTest::newRow("fb no dns") << "facebook" << 0 << QJsonObject() << "Host graph.facebook.com not found" << "" << "failed" << "not_sent" << true;
        QTest::newRow("fb file") << "facebook" << 0 << QJsonObject() << "cannot read C:/media/spot.mp4" << "" << "failed" << "not_sent" << true;
        QTest::newRow("fb 500") << "facebook" << 500 << graph(1) << "" << "" << "unknown" << "server_error" << false;
        QTest::newRow("fb 503 html") << "facebook" << 503 << QJsonObject() << "" << "" << "unknown" << "server_error" << false;
        QTest::newRow("fb transient flag") << "facebook" << 400 << graph(9007, true) << "" << "" << "unknown" << "transient" << false;
        QTest::newRow("fb code 2") << "facebook" << 400 << graph(2) << "" << "" << "unknown" << "transient" << false;
        QTest::newRow("fb duplicate 506") << "facebook" << 400 << graph(506) << "" << "" << "unknown" << "duplicate" << false;
        QTest::newRow("fb token 190") << "facebook" << 400 << graph(190) << "" << "" << "failed" << "auth" << false;
        QTest::newRow("fb permission 200") << "facebook" << 403 << graph(200) << "" << "" << "failed" << "permission" << false;
        QTest::newRow("fb rate 4") << "facebook" << 400 << graph(4) << "" << "" << "failed" << "rate" << true;
        QTest::newRow("fb rate 429") << "facebook" << 429 << QJsonObject() << "" << "" << "failed" << "rate" << true;
        QTest::newRow("fb blocked 368") << "facebook" << 400 << graph(368) << "" << "" << "failed" << "blocked" << false;
        QTest::newRow("fb invalid 100") << "facebook" << 400 << graph(100) << "" << "" << "failed" << "content" << false;
        QTest::newRow("fb other 4xx") << "facebook" << 404 << QJsonObject() << "" << "" << "failed" << "rejected" << true;
        QTest::newRow("web 201 id") << "website" << 201 << QJsonObject{{"id", 77}} << "" << "77" << "published" << "" << false;
        QTest::newRow("web 201 id 0") << "website" << 201 << QJsonObject() << "" << "0" << "unknown" << "bad_answer" << false;
        QTest::newRow("web timeout") << "website" << 0 << QJsonObject() << "timed out" << "" << "unknown" << "no_answer" << false;
        QTest::newRow("web 502") << "website" << 502 << QJsonObject() << "" << "" << "unknown" << "server_error" << false;
        QTest::newRow("web 401") << "website" << 401 << QJsonObject() << "" << "" << "failed" << "auth" << false;
        QTest::newRow("web 400") << "website" << 400 << QJsonObject() << "" << "" << "failed" << "content" << false;
        QTest::newRow("web 429") << "website" << 429 << QJsonObject() << "" << "" << "failed" << "rate" << true;
        QTest::newRow("web 409") << "website" << 409 << QJsonObject() << "" << "" << "failed" << "rejected" << true;
    }
    void classify() {
        QFETCH(QString, channel); QFETCH(int, http); QFETCH(QJsonObject, json); QFETCH(QString, netError); QFETCH(QString, remoteId);
        QFETCH(QString, state); QFETCH(QString, cls); QFETCH(bool, retryable);
        const SendResult r = channel == "facebook" ? classifyFacebook(http, json, "err", netError, remoteId) : classifyWebsite(http, json, "err", netError, remoteId);
        QCOMPARE(stateName(r.state), state); QCOMPARE(r.errorClass, cls); QCOMPARE(r.retryable, retryable);
        QVERIFY(r.state != Outcome::Unknown || !r.retryable);                       // an unknown outcome is never retried
    }
    void notSentAndBackoff() {
        const SendResult r = notSent("Facebook Page not configured");
        QCOMPARE(r.state, Outcome::Failed); QCOMPARE(r.errorClass, QString("not_sent")); QVERIFY(r.retryable);
        const qint64 t = now();
        QCOMPARE(retryAtMs(1, t), t + 2 * kMin);
        QCOMPARE(retryAtMs(2, t), t + 10 * kMin);
        QCOMPARE(retryAtMs(3, t), qint64(0));                                       // kMaxAttempts used up
        QVERIFY(mayAutoRetry(QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 2}}));
        QVERIFY(!mayAutoRetry(QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 3}}));
        QVERIFY(!mayAutoRetry(QJsonObject{{"state", "failed"}, {"retryable", false}, {"attempts", 1}}));
        QVERIFY(!mayAutoRetry(QJsonObject{{"state", "failed"}, {"attempts", 1}}));   // a legacy failure without a class: a person decides
        QVERIFY(!mayAutoRetry(QJsonObject{{"state", "unknown"}, {"retryable", true}, {"attempts", 1}}));
    }

    // ---------------------------------------------------------------- reconcile: matching and verdicts
    void facebookMatch_data() {
        QTest::addColumn<QJsonArray>("data");
        QTest::addColumn<QString>("expectId");
        const QString later = utcIso(now() - 2 * kMin) + "+0000";          // Graph's created_time format
        const QString older = utcIso(now() - 90 * kMin) + "+0000";
        QTest::newRow("same message") << QJsonArray{QJsonObject{{"id", "9_1"}, {"message", "Other post"}, {"created_time", later}}, QJsonObject{{"id", "9_2"}, {"message", "Main Street is CLOSED  until 6 pm.\n\nMade with AI assistance."}, {"created_time", later}}} << "9_2";
        QTest::newRow("video description") << QJsonArray{QJsonObject{{"id", "v1"}, {"description", "main street is closed until 6 pm. made with ai assistance."}, {"created_time", later}}} << "v1";
        QTest::newRow("different words") << QJsonArray{QJsonObject{{"id", "9_3"}, {"message", "Main Street is open."}, {"created_time", later}}} << "";
        QTest::newRow("older identical post") << QJsonArray{QJsonObject{{"id", "9_4"}, {"message", "Main Street is closed until 6 pm. Made with AI assistance."}, {"created_time", older}}} << "";
        QTest::newRow("empty page") << QJsonArray() << "";
    }
    void facebookMatch() {
        QFETCH(QJsonArray, data); QFETCH(QString, expectId);
        const QString sha = textSha256("Main Street is closed until 6 pm.\n\nMade with AI assistance.");
        const QJsonObject hit = findFacebookPost(data, sha, now() - 20 * kMin);
        QCOMPARE(hit.value("id").toString(), expectId);
        if (!expectId.isEmpty()) { QVERIFY(!hit.value("permalink").toString().isEmpty()); QVERIFY(hit.value("createdMs").toDouble() > 0); }
        QVERIFY(findFacebookPost(data, QString(), 0).isEmpty());                   // no recorded hash: nothing can be matched
    }
    void websiteMatch_data() {
        QTest::addColumn<QJsonArray>("rows");
        QTest::addColumn<QString>("expectId");
        const QString recent = utcIso(now() - 2 * kMin);                   // WordPress date_gmt: UTC without a zone
        const QString old = utcIso(now() - 300 * kMin);
        QTest::newRow("raw title") << QJsonArray{QJsonObject{{"id", 11}, {"title", QJsonObject{{"raw", "Road closed on Main Street"}, {"rendered", "x"}}}, {"date_gmt", recent}, {"link", "https://s/11"}}} << "11";
        QTest::newRow("rendered entities") << QJsonArray{QJsonObject{{"id", 12}, {"title", QJsonObject{{"rendered", "Driver&#8217;s road closed on Main Street"}}}, {"date_gmt", recent}}} << "12";
        QTest::newRow("draft: modified_gmt") << QJsonArray{QJsonObject{{"id", 13}, {"title", QJsonObject{{"raw", "Road closed on Main Street"}}}, {"date_gmt", QJsonValue()}, {"modified_gmt", recent}}} << "13";
        QTest::newRow("older same title") << QJsonArray{QJsonObject{{"id", 14}, {"title", QJsonObject{{"raw", "Road closed on Main Street"}}}, {"date_gmt", old}}} << "";
        QTest::newRow("undated") << QJsonArray{QJsonObject{{"id", 15}, {"title", QJsonObject{{"raw", "Road closed on Main Street"}}}}} << "";
        QTest::newRow("other title") << QJsonArray{QJsonObject{{"id", 16}, {"title", QJsonObject{{"raw", "Road open"}}}, {"date_gmt", recent}}} << "";
    }
    void websiteMatch() {
        QFETCH(QJsonArray, rows); QFETCH(QString, expectId);
        const QString title = rows.isEmpty() ? QString() : (QTest::currentDataTag() == QByteArray("rendered entities") ? QString("Driver's road closed on Main Street") : QString("Road closed on Main Street"));
        QCOMPARE(findWebsitePost(rows, title, now() - 20 * kMin).value("id").toString(), expectId);
    }
    void reconcileVerdicts_data() {
        QTest::addColumn<bool>("checked");
        QTest::addColumn<bool>("found");
        QTest::addColumn<int>("ageMin");
        QTest::addColumn<int>("verdict");
        QTest::newRow("found") << true << true << 1 << int(Verdict::Published);
        QTest::newRow("not yet") << true << false << 5 << int(Verdict::StillUnknown);
        QTest::newRow("give up") << true << false << 31 << int(Verdict::NotFound);
        QTest::newRow("could not look, young") << false << false << 1 << int(Verdict::StillUnknown);
        QTest::newRow("could not look, old") << false << false << 600 << int(Verdict::StillUnknown);   // "I could not look" is never a green light
    }
    void reconcileVerdicts() {
        QFETCH(bool, checked); QFETCH(bool, found); QFETCH(int, ageMin); QFETCH(int, verdict);
        QCOMPARE(int(reconcileVerdict(checked, found, qint64(ageMin) * kMin)), verdict);
    }

    // ---------------------------------------------------------------- restart recovery (persist 'publishing' before the call)
    void recoverInterrupted_data() {
        QTest::addColumn<QJsonObject>("item");
        QTest::addColumn<bool>("changed");
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("fbState");
        const double t0 = double(now() - 3 * kMin);
        QJsonObject marked = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "publishing"}, {"attempts", 1}, {"attemptStartedMs", t0}, {"msgSha256", "abc"}}}}); marked["status"] = "publishing";
        QJsonObject legacy = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "pending"}}}}); legacy["status"] = "publishing"; legacy["updatedMs"] = t0;
        QJsonObject legacyFailed = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"attempts", 1}, {"lastTryMs", t0}}}}); legacyFailed["status"] = "publishing";
        QJsonObject allDone = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "published"}, {"postedMs", t0}}}}); allDone["status"] = "publishing";
        QJsonObject mixed = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "publishing"}, {"attemptStartedMs", t0}}}, {"website", QJsonObject{{"state", "published"}, {"postedMs", t0}}}}); mixed["status"] = "publishing";
        QJsonObject approved = withChannels(QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"attempts", 1}, {"retryable", true}}}});
        QTest::newRow("channel mark") << marked << true << "reconcile" << "unknown";
        QTest::newRow("legacy item mark") << legacy << true << "reconcile" << "unknown";
        QTest::newRow("legacy failed in flight") << legacyFailed << true << "reconcile" << "unknown";
        QTest::newRow("finished before finish") << allDone << true << "published" << "published";
        QTest::newRow("one of two in flight") << mixed << true << "reconcile" << "unknown";
        QTest::newRow("nothing in flight") << approved << false << "approved" << "failed";
    }
    void recoverInterrupted() {
        QFETCH(QJsonObject, item); QFETCH(bool, changed); QFETCH(QString, status); QFETCH(QString, fbState);
        bool ch = false; const QJsonObject r = pubrails::recoverInterrupted(item, now(), &ch);
        QCOMPARE(ch, changed); QCOMPARE(r.value("status").toString(), status);
        const QJsonObject fb = r.value("perChannel").toObject().value("facebook").toObject();
        QCOMPARE(fb.value("state").toString(), fbState);
        if (fbState == "unknown") {
            QVERIFY(fb.value("attemptStartedMs").toDouble() > 0); QVERIFY(fb.value("attempts").toInt() >= 1); QCOMPARE(fb.value("errorClass").toString(), QString("interrupted"));
            QVERIFY(!mayAutoRetry(fb)); QCOMPARE(busyChannels(r), QStringList{"facebook"});
        }
        if (item.value("perChannel").toObject().contains("website")) QCOMPARE(r.value("perChannel").toObject().value("website").toObject().value("state").toString(), QString("published"));
        if (item.value("perChannel").toObject().value("facebook").toObject().contains("msgSha256")) QCOMPARE(fb.value("msgSha256").toString(), QString("abc"));
    }

    // ---------------------------------------------------------------- the item ladder
    void aggregate_data() {
        QTest::addColumn<QJsonObject>("states");
        QTest::addColumn<QString>("status");
        QTest::addColumn<int>("retryInMin");
        const double t = double(now());
        QTest::newRow("all published") << QJsonObject{{"facebook", QJsonObject{{"state", "published"}}}, {"website", QJsonObject{{"state", "published"}}}} << "published" << 0;
        QTest::newRow("unknown wins over failed") << QJsonObject{{"facebook", QJsonObject{{"state", "unknown"}}}, {"website", QJsonObject{{"state", "failed"}, {"retryable", false}, {"attempts", 3}}}} << "reconcile" << 0;
        QTest::newRow("still sending") << QJsonObject{{"facebook", QJsonObject{{"state", "publishing"}}}, {"website", QJsonObject{{"state", "unknown"}}}} << "publishing" << 0;
        QTest::newRow("retry after 2 min") << QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 1}, {"lastTryMs", t}}}} << "approved" << 2;
        QTest::newRow("retry after 10 min") << QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 2}, {"lastTryMs", t}}}, {"website", QJsonObject{{"state", "published"}}}} << "approved" << 10;
        QTest::newRow("attempts used up") << QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 3}, {"lastTryMs", t}}}} << "failed" << 0;
        QTest::newRow("definitive") << QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"retryable", false}, {"attempts", 1}}}} << "failed" << 0;
        QTest::newRow("one cannot retry") << QJsonObject{{"facebook", QJsonObject{{"state", "failed"}, {"retryable", true}, {"attempts", 1}, {"lastTryMs", t}}}, {"website", QJsonObject{{"state", "failed"}, {"errorClass", "auth"}, {"attempts", 1}}}} << "failed" << 0;
    }
    void aggregate() {
        QFETCH(QJsonObject, states); QFETCH(QString, status); QFETCH(int, retryInMin);
        qint64 next = -1; QStringList errs;
        QCOMPARE(aggregateStatus(withChannels(states), &next, &errs), status);
        QCOMPARE(next, retryInMin > 0 ? now() + qint64(retryInMin) * kMin : qint64(0));
        QCOMPARE(errs.isEmpty(), status == "published");
    }
    void actionBlocks_data() {
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("action");
        QTest::addColumn<bool>("blocked");
        for (const char* a : {"approve", "hold", "skip", "schedule", "retry", "delete", "send", "revise", "resolve"}) QTest::newRow(qPrintable(QString("publishing/") + a)) << "publishing" << a << true;
        for (const char* a : {"approve", "hold", "skip", "schedule", "retry", "delete", "send", "revise", "correct", "retract"}) QTest::newRow(qPrintable(QString("reconcile/") + a)) << "reconcile" << a << true;
        QTest::newRow("reconcile/resolve") << "reconcile" << "resolve" << false;
        QTest::newRow("reconcile/reconcile") << "reconcile" << "reconcile" << false;
        QTest::newRow("approved/resolve") << "approved" << "resolve" << true;
        QTest::newRow("approved/approve") << "approved" << "approve" << false;
        QTest::newRow("failed/retry") << "failed" << "retry" << false;
        QTest::newRow("published/correct") << "published" << "correct" << false;
    }
    void actionBlocks() {
        QFETCH(QString, status); QFETCH(QString, action); QFETCH(bool, blocked);
        QCOMPARE(!actionBlock(status, action).isEmpty(), blocked);
    }

    // ---------------------------------------------------------------- the two acceptance stories, end to end on the pure rails
    void storyHourlyCapRefusesAFakePublish() {
        // four Page posts this hour (gaps fine): a fifth, correctly approved, is refused by the hourly rail before any network call
        const qint64 n = now(), d0 = localDayStartMs(n);
        QJsonArray q{posted("facebook", "published", 12), posted("facebook", "published", 24), posted("facebook", "published", 36), posted("facebook", "published", 48)};
        QJsonObject next = baseItem(); next["channels"] = QJsonArray{"facebook"}; next["approval"] = bindApproval(next, "ops", n); q.append(next);
        QVERIFY(approvalProblem(next).isEmpty());
        const QList<Check> checks = rateChecks("facebook", statsFor(q, "facebook", n, d0), limitsFor("facebook", QJsonObject()), n, d0);
        QStringList failing; for (const Check& c : checks) if (!c.pass) failing << c.name;
        QCOMPARE(failing, QStringList{"rail:facebook_hour"});
        // and a config cannot lift it
        QVERIFY(!rateChecks("facebook", statsFor(q, "facebook", n, d0), limitsFor("facebook", QJsonObject{{"facebook", QJsonObject{{"perHour", 100}}}}), n, d0).first().pass);
    }
    void storyUnknownGoesToReconcileNotRetry() {
        // Graph answers 500 to the post: unknown -> the item is 'reconcile', nothing may send to that channel, no automatic retry
        QJsonObject it = baseItem(); it["channels"] = QJsonArray{"facebook"};
        const SendResult r = classifyFacebook(500, QJsonObject{{"error", QJsonObject{{"code", 2}, {"message", "An unexpected error has occurred."}}}}, "Graph API: HTTP 500", QString(), QString());
        QCOMPARE(r.state, Outcome::Unknown);
        QJsonObject one{{"state", stateName(r.state)}, {"attempts", 1}, {"attemptStartedMs", double(now())}, {"lastTryMs", double(now())}, {"retryable", r.retryable}, {"errorClass", r.errorClass}};
        it["perChannel"] = QJsonObject{{"facebook", one}};
        qint64 next = 0;
        QCOMPARE(aggregateStatus(it, &next), QString("reconcile"));
        QCOMPARE(next, qint64(0));
        QVERIFY(!mayAutoRetry(one));
        QVERIFY(!actionBlock("reconcile", "send").isEmpty());
        QVERIFY(!actionBlock("reconcile", "retry").isEmpty());
        QCOMPARE(busyChannels(it), QStringList{"facebook"});
        // the unknown attempt uses up the budget like a post (it may have gone out)
        QCOMPARE(statsFor(QJsonArray{it}, "facebook", now(), localDayStartMs(now())).hour, 1);
    }
};

QTEST_GUILESS_MAIN(TestPubRails)
#include "test_pubrails.moc"
