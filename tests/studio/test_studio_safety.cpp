// ST-15 safety module: the real-people guard (mcp/media/realpeople.ts), the spot rules (mcp/spot/safety.ts), voice
// consent (mcp/voice/consent.ts) with the send-time re-check, the provider-layer hook that blocks an image / video /
// cloned-speech body before it is sent, and secret redaction (mcp/core/util.ts redactValue).
// The cases are the StarMedia TS tests' own (test/mcp-media.ts, mcp-fix-media.ts S9, mcp-spot.ts, mcp-fix-spot.ts,
// mcp-voice.ts, mcp-integration.ts, mcp-core.ts redactValue), plus Studio-only ones (fail closed, the hook, the log).
// Hermetic: a mock OpenRouter on an ephemeral loopback port; temp dirs; no model API.
#include <QtTest>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimeZone>

#include "core/datafiles.h"
#include "core/log.h"
#include "mockopenrouter.h"
#include "safety/consent.h"
#include "safety/guards.h"
#include "safety/realpeople.h"
#include "safety/redact.h"

using namespace studio;
using namespace studio::safety;
using namespace Qt::StringLiterals;

namespace {

// 2026-10-26 12:00 UTC: "now" for the consent dates (the TS tests use the real clock with a 2026-09-30 record).
const qint64 kNow = QDateTime(QDate(2026, 10, 26), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();

ConsentInput janeConsent() {
    return ConsentInput{QStringLiteral("Jane Doe"), QStringLiteral("I, Jane Doe, agree to StarMedia cloning my voice for narration of our station promos."),
                        QStringLiteral("Jane Doe"), QStringLiteral("2026-09-30")};
}
QJsonObject storedConsent(bool confirmed = true) {
    ConsentRecord r;
    const auto refused = checkConsent(janeConsent(), kNow, &r);
    if (refused) qFatal("test consent refused: %s", qPrintable(refused->message));
    r.recordedAt = QStringLiteral("2026-10-01T10:00:00.000Z");
    r.confirmedByUser = confirmed;
    r.session = QStringLiteral("ui");
    return r.toJson();
}
QString codeOf(const std::optional<ConsentRefusal>& r) { return r ? r->code : QString(); }
QString msgOf(const std::optional<ConsentRefusal>& r) { return r ? r->message : QString(); }

// The send path the OpenRouter client (ST-07) runs: the hook first, then the wire. Returns the HTTP status, or -1 when
// the hook refused (nothing sent).
int guardedPost(QNetworkAccessManager& nam, const test::MockOpenRouter& mock, const QString& path, const QJsonObject& body,
                const SendContext& ctx, Verdict* verdict = nullptr) {
    const Verdict v = checkOutgoing(path, body, ctx);
    if (verdict) *verdict = v;
    if (v.refuse) return -1;
    QNetworkRequest rq{QUrl(mock.apiUrl() + path)};
    rq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    rq.setRawHeader("Authorization", "Bearer sk-or-v1-testtesttesttesttesttest");
    QNetworkReply* r = nam.post(rq, QJsonDocument(body).toJson(QJsonDocument::Compact));
    (void)QTest::qWaitFor([r] { return r->isFinished(); }, 15000);
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r->deleteLater();
    return status;
}

// What the StarMedia TS (mcp/media/realpeople.ts realPersonCheck / publicFiguresIn / publicFigureInName and
// mcp/spot/safety.ts realPeopleProblem, commit 12c10f1 of 2026-10-26) answers for a corpus of edge cases: titles,
// places, stage and ambiguous names in either case, look-alike and full-width letters, the dog breed. Generated once
// with node --experimental-strip-types over copies of those two files; the C++ must give the same answers.
const char* const kTsGolden =
    "[\n"
    "{\"t\": \"Doctor Who fans at a convention, photo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Dr. Jane Foster in a lab, photorealistic\", \"rpc\": true, \"names\": [\"Dr. Jane Foster in\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Queen Latifah style singer on stage\", \"rpc\": true, \"names\": [\"Queen Latifah style singer\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"a portrait of Pope Leo blessing a crowd, oil painting\", \"rpc\": false, \"names\": [\"pope leo\", \"Pope Leo blessing\"], \"sty\": true, \"pf\": [\"pope leo\"], \"nm\": \"pope leo\", \"spot\": \"names a real public figure (\\\"pope leo\\\")\"},\n"
    "{\"t\": \"Prince William Street mural\", \"rpc\": true, \"names\": [\"prince william\"], \"sty\": false, \"pf\": [\"prince william\"], \"nm\": \"prince william\", \"spot\": \"names a real public figure (\\\"prince william\\\")\"},\n"
    "{\"t\": \"The Rock and Drake at a BBQ\", \"rpc\": true, \"names\": [\"the rock\", \"drake\"], \"sty\": false, \"pf\": [\"the rock\", \"drake\"], \"nm\": \"the rock\", \"spot\": \"names a real public figure (\\\"the rock\\\")\"},\n"
    "{\"t\": \"the rock and drake at a bbq\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"the rock\", \"spot\": \"\"},\n"
    "{\"t\": \"Michael Jordan dunking, 35mm film still\", \"rpc\": true, \"names\": [\"michael jordan\", \"Michael Jordan\"], \"sty\": false, \"pf\": [\"michael jordan\"], \"nm\": \"michael jordan\", \"spot\": \"names a real public figure (\\\"michael jordan\\\")\"},\n"
    "{\"t\": \"a sketch of Michael Jordan dunking\", \"rpc\": false, \"names\": [\"michael jordan\", \"Michael Jordan\"], \"sty\": true, \"pf\": [\"michael jordan\"], \"nm\": \"michael jordan\", \"spot\": \"names a real public figure (\\\"michael jordan\\\")\"},\n"
    "{\"t\": \"Mrs. Robinson in a kitchen, candid\", \"rpc\": true, \"names\": [\"Mrs. Robinson in\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Ms. Smith from accounting, illustration\", \"rpc\": false, \"names\": [\"Ms. Smith from accounting\"], \"sty\": true, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"CEO Tim Cook unveils a phone\", \"rpc\": true, \"names\": [\"tim cook\", \"CEO Tim Cook unveils\"], \"sty\": false, \"pf\": [\"tim cook\"], \"nm\": \"tim cook\", \"spot\": \"names a real public figure (\\\"tim cook\\\")\"},\n"
    "{\"t\": \"mayor Pete at the park, photo\", \"rpc\": true, \"names\": [\"mayor Pete at the\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Taylor Street Bakery storefront, photo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Emma Watson-style actress, realistic\", \"rpc\": true, \"names\": [\"emma watson\", \"Emma Watson-style\"], \"sty\": false, \"pf\": [\"emma watson\"], \"nm\": \"emma watson\", \"spot\": \"names a real public figure (\\\"emma watson\\\")\"},\n"
    "{\"t\": \"Sarah Connor running, cinematic still\", \"rpc\": true, \"names\": [\"Sarah Connor\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Jordan Peterson lecture, footage\", \"rpc\": true, \"names\": [\"Jordan Peterson\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"a lookalike of a pop singer\", \"rpc\": true, \"names\": [\"lookalike\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a deepfake / look-alike (\\\"lookalike\\\")\"},\n"
    "{\"t\": \"doppelg\\u00e4nger in a mirror\", \"rpc\": true, \"names\": [\"doppelg\\u00e4nger\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a deepfake / look-alike (\\\"doppelg\\u00e4nger\\\")\"},\n"
    "{\"t\": \"paparazzi shot of a beach\", \"rpc\": true, \"names\": [\"paparazzi\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Kim Jong-Un at a parade, cartoon\", \"rpc\": false, \"names\": [\"kim jong un\"], \"sty\": true, \"pf\": [\"kim jong un\"], \"nm\": \"kim jong un\", \"spot\": \"names a real public figure (\\\"kim jong un\\\")\"},\n"
    "{\"t\": \"Kim Jong Un at a parade\", \"rpc\": true, \"names\": [\"kim jong un\"], \"sty\": false, \"pf\": [\"kim jong un\"], \"nm\": \"kim jong un\", \"spot\": \"names a real public figure (\\\"kim jong un\\\")\"},\n"
    "{\"t\": \"BIDEN 2024 sign in a yard\", \"rpc\": true, \"names\": [\"biden\"], \"sty\": false, \"pf\": [\"biden\"], \"nm\": \"biden\", \"spot\": \"names a real public figure (\\\"biden\\\")\"},\n"
    "{\"t\": \"a biden-themed mug\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"biden\", \"spot\": \"\"},\n"
    "{\"t\": \"Biden-themed mug\", \"rpc\": true, \"names\": [\"biden\"], \"sty\": false, \"pf\": [\"biden\"], \"nm\": \"biden\", \"spot\": \"names a real public figure (\\\"biden\\\")\"},\n"
    "{\"t\": \"Zelenskyy speaking\", \"rpc\": true, \"names\": [\"zelenskyy\"], \"sty\": false, \"pf\": [\"zelenskyy\"], \"nm\": \"zelenskyy\", \"spot\": \"names a real public figure (\\\"zelenskyy\\\")\"},\n"
    "{\"t\": \"a photo of the first lady\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks to depict a real office-holder (\\\"photo of the first lady\\\")\"},\n"
    "{\"t\": \"render of former US president\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks to depict a real office-holder (\\\"render of former US president\\\")\"},\n"
    "{\"t\": \"showing the governor shaking hands\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks to depict a real office-holder (\\\"showing the governor\\\")\"},\n"
    "{\"t\": \"recognizable politicians at a dinner\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a depiction of a real, identifiable person (\\\"recognizable politicians\\\")\"},\n"
    "{\"t\": \"a famous singer, lifelike\", \"rpc\": true, \"names\": [\"famous singer\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a realistic image of a celebrity (\\\"famous singer, lifelike\\\")\"},\n"
    "{\"t\": \"Christmas Morning in Main Street, photo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Grand Hotel lobby, photo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Mary Poppins flying, photo\", \"rpc\": true, \"names\": [\"Mary Poppins\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Detective Pikachu in a city, 3d animated\", \"rpc\": false, \"names\": [\"Detective Pikachu in\"], \"sty\": true, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Judge Judy in court, photo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"King Charles Cavalier Spaniels playing\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"King Charles spaniel\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"king charles in london\", \"rpc\": true, \"names\": [\"king charles\"], \"sty\": false, \"pf\": [\"king charles\"], \"nm\": \"king charles\", \"spot\": \"names a real public figure (\\\"king charles\\\")\"},\n"
    "{\"t\": \"Obama Obama obama\", \"rpc\": true, \"names\": [\"obama\"], \"sty\": false, \"pf\": [\"obama\"], \"nm\": \"obama\", \"spot\": \"names a real public figure (\\\"obama\\\")\"},\n"
    "{\"t\": \"An anchor on the news desk, photorealistic\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Anchor Dave Smith reads news\", \"rpc\": true, \"names\": [\"Anchor Dave Smith reads\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Mr Beast giving away cash\", \"rpc\": true, \"names\": [\"mr beast\"], \"sty\": false, \"pf\": [\"mr beast\"], \"nm\": \"mr beast\", \"spot\": \"names a real public figure (\\\"mr beast\\\")\"},\n"
    "{\"t\": \"mrbeast thumbnail\", \"rpc\": true, \"names\": [\"mrbeast\"], \"sty\": false, \"pf\": [\"mrbeast\"], \"nm\": \"mrbeast\", \"spot\": \"names a real public figure (\\\"mrbeast\\\")\"},\n"
    "{\"t\": \"\\uff34\\uff41\\uff59\\uff4c\\uff4f\\uff52 \\uff33\\uff57\\uff49\\uff46\\uff54\", \"rpc\": true, \"names\": [\"taylor swift\", \"Taylor Swift\"], \"sty\": false, \"pf\": [\"taylor swift\"], \"nm\": \"taylor swift\", \"spot\": \"names a real public figure (\\\"taylor swift\\\")\"},\n"
    "{\"t\": \"T\\u0430ylor Sw\\u0456ft photo\", \"rpc\": true, \"names\": [\"taylor swift\", \"Taylor Swift\"], \"sty\": false, \"pf\": [\"taylor swift\"], \"nm\": \"taylor swift\", \"spot\": \"names a real public figure (\\\"taylor swift\\\")\"},\n"
    "{\"t\": \"george w. bush painting\", \"rpc\": false, \"names\": [\"george w bush\"], \"sty\": true, \"pf\": [\"george w bush\"], \"nm\": \"george w bush\", \"spot\": \"names a real public figure (\\\"george w bush\\\")\"},\n"
    "{\"t\": \"J.D. Vance rally, photo\", \"rpc\": true, \"names\": [\"j d vance\"], \"sty\": false, \"pf\": [\"j d vance\"], \"nm\": \"j d vance\", \"spot\": \"names a real public figure (\\\"j d vance\\\")\"},\n"
    "{\"t\": \"Elvis-impersonator at a wedding\", \"rpc\": true, \"names\": [\"impersonator\"], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a deepfake / look-alike (\\\"impersonator\\\")\"},\n"
    "{\"t\": \"a deep fake of a ceo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"asks for a deepfake / look-alike (\\\"deep fake\\\")\"},\n"
    "{\"t\": \"face-swap app demo\", \"rpc\": false, \"names\": [], \"sty\": false, \"pf\": [], \"nm\": \"\", \"spot\": \"\"},\n"
    "{\"t\": \"Pewdiepie gaming setup\", \"rpc\": true, \"names\": [\"pewdiepie\"], \"sty\": false, \"pf\": [\"pewdiepie\"], \"nm\": \"pewdiepie\", \"spot\": \"names a real public figure (\\\"pewdiepie\\\")\"},\n"
    "{\"t\": \"Adele concert, photo\", \"rpc\": true, \"names\": [\"adele\"], \"sty\": false, \"pf\": [\"adele\"], \"nm\": \"adele\", \"spot\": \"names a real public figure (\\\"adele\\\")\"},\n"
    "{\"t\": \"adele concert\", \"rpc\": true, \"names\": [\"adele\"], \"sty\": false, \"pf\": [\"adele\"], \"nm\": \"adele\", \"spot\": \"names a real public figure (\\\"adele\\\")\"},\n"
    "{\"t\": \"a sad adele\", \"rpc\": true, \"names\": [\"adele\"], \"sty\": false, \"pf\": [\"adele\"], \"nm\": \"adele\", \"spot\": \"names a real public figure (\\\"adele\\\")\"}\n"
    "]\n";

} // namespace

class TestStudioSafety : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void dataLoads();
    void foldAndNormalise();
    void publicFiguresInText();
    void nameFields();
    void pictureRule();
    void pictureRuleLookalikeLetters();
    void parityWithTypeScript();
    void spotRules();
    void eachToolKeepsItsStrictestRule();
    void consentRecord();
    void consentPublicFigures();
    void consentRecheckAtSendTime();
    void failClosed();
    void hookClassifiesRequests();
    void hookBlocksBeforeSend();
    void hookReadsStyleBansAsBans();
    void redaction();
    void logsNeverCarrySecrets();

private:
    QTemporaryDir m_logDir;
};

void TestStudioSafety::initTestCase() {
    QVERIFY(m_logDir.isValid());
    log::Options o;
    o.dir = m_logDir.path();
    o.minLevel = log::Level::Debug;
    QVERIFY(log::start(o));
}

void TestStudioSafety::cleanupTestCase() { log::stop(); }

void TestStudioSafety::dataLoads() {
    QString why;
    QVERIFY2(listsReady(&why), qPrintable(why));
    const QJsonObject d = data::load(u"realpeople");
    QVERIFY(d.value(u"publicFigures").toArray().size() > 150);
    // the lists the TS keeps in code are all there
    QVERIFY(d.value(u"stageNames").toArray().contains(QStringLiteral("beyonce")));
    QVERIFY(d.value(u"ambiguousNames").toArray().contains(QStringLiteral("the rock")));
}

void TestStudioSafety::foldAndNormalise() {
    QCOMPARE(foldText(u"Bаrack Obаma"), QStringLiteral("Barack Obama"));           // Cyrillic а
    QCOMPARE(foldText(u"Οbama"), QStringLiteral("Obama"));                              // Greek Ο
    QCOMPARE(foldText(u"Barack O​bama"), QStringLiteral("Barack Obama"));               // zero-width space
    QCOMPARE(foldText(u"Elon M­usk"), QStringLiteral("Elon Musk"));                     // soft hyphen
    QCOMPARE(foldText(u"ＢＡＲＡＣＫ"), QStringLiteral("BARACK"));   // full-width (NFKC)
    QCOMPARE(foldText(u"a️b\U000E0101c"), QStringLiteral("abc"));                       // variation selectors
    QCOMPARE(normName(u"  BEYONCÉ  Knowles!! "), QStringLiteral("beyonce knowles"));
    QCOMPARE(normName(u"Alexandria Ocasio-Cortez"), QStringLiteral("alexandria ocasio cortez"));
    QCOMPARE(normName(u"J.D. Vance"), QStringLiteral("j d vance"));
}

void TestStudioSafety::publicFiguresInText() {
    // mcp-integration.ts: free text, ambiguous names only when Capitalised; the dog breed is a dog
    QVERIFY(findPublicFigure(u"a trump card on green felt").isEmpty());
    QVERIFY(findPublicFigure(u"a drake on the pond").isEmpty());
    QCOMPARE(findPublicFigure(u"Trump at a rally"), QStringLiteral("trump"));
    QVERIFY(findPublicFigure(u"a King Charles spaniel on a sofa").isEmpty());
    QVERIFY(findPublicFigure(u"two king charles cavalier spaniels").isEmpty());
    QCOMPARE(findPublicFigure(u"King Charles at the counter"), QStringLiteral("king charles"));
    QCOMPARE(findPublicFigure(u"The Rock lifts weights"), QStringLiteral("the rock"));
    QVERIFY(findPublicFigure(u"climbing the rock at dawn").isEmpty());
    QVERIFY(findPublicFigure(u"The rock at dawn").isEmpty());          // "rock" not Capitalised
    QCOMPARE(findPublicFigure(u"a mural of Beyoncé"), QStringLiteral("beyonce"));   // stage name, accent folded
    QCOMPARE(findPublicFigure(u"OPRAH on a couch"), QStringLiteral("oprah"));          // stage names: any case
    QVERIFY(findPublicFigure(u"Obamacare paperwork").isEmpty());        // whole words only
    QVERIFY(findPublicFigure(u"Mr. Trumpet plays").isEmpty());
    // order: full names (list order), then stage names, then ambiguous; no duplicates
    QCOMPARE(publicFiguresIn(u"Taylor Swift meets Barack Obama and Obama and Rihanna"),
             (QStringList{QStringLiteral("barack obama"), QStringLiteral("taylor swift"), QStringLiteral("rihanna"), QStringLiteral("obama")}));
    QCOMPARE(publicFiguresIn(u"Barack Obama and Rihanna", true), QStringList{QStringLiteral("barack obama")});
    // capitalised Cyrillic look-alike surname still counts
    QCOMPARE(findPublicFigure(u"Оbama waving"), QStringLiteral("obama"));
}

void TestStudioSafety::nameFields() {
    // mcp-media.ts / mcp-integration.ts
    QVERIFY(looksLikePublicFigure(u"Elon Musk"));
    QVERIFY(looksLikePublicFigure(u"  barack OBAMA "));
    QVERIFY(!looksLikePublicFigure(u"Jane Q. Example"));
    QVERIFY(looksLikePublicFigure(u"trump family"));                 // NAME field: ambiguous names in any case
    QVERIFY(!looksLikePublicFigure(u"Jane Doe"));
    QCOMPARE(publicFigureInName(u"elon musk"), QStringLiteral("elon musk"));
    QCOMPARE(publicFigureInName(u"Narrator Zendaya v2"), QStringLiteral("zendaya"));
    QVERIFY(publicFigureInName(u"   ").isEmpty());
    QVERIFY(!looksLikePublicFigure(u"King Charles spaniel breeder"));
}

void TestStudioSafety::pictureRule() {
    // mcp-media.ts
    QVERIFY(realPersonCheck(u"a photo of Taylor Swift at a concert").refuse);
    QVERIFY(realPersonCheck(u"Elon Musk shaking hands with a robot").refuse);
    QVERIFY(!realPersonCheck(u"a friendly cartoon caricature of Taylor Swift playing guitar").refuse);
    QVERIFY(realPersonCheck(u"Senator Jane Doe speaking at a podium").refuse);
    QVERIFY(realPersonCheck(u"realistic likeness of my neighbour").refuse);
    QVERIFY(!realPersonCheck(u"a barista named Maya pours coffee in a warm shop, photorealistic").refuse);
    QVERIFY(realPersonCheck(u"John Smith headshot, studio photo").refuse);
    QVERIFY(!realPersonCheck(u"John Smith as a watercolour storybook hero").refuse);
    QVERIFY(!realPersonCheck(u"Henry County courthouse at dusk, news photo, Main Street Coffee storefront").refuse);
    QVERIFY(!realPersonCheck(u"the mayor of a small town cuts a ribbon, illustration").refuse);
    // mcp-integration.ts: names from every old list reach the picture rule
    QVERIFY(realPersonCheck(u"Abraham Lincoln at a desk, photo").refuse);
    QVERIFY(realPersonCheck(u"Sydney Sweeney holding a mug").refuse);
    QVERIFY(!realPersonCheck(u"a King Charles spaniel, photo").refuse);
    // the style is read with the text; "photo-real" cancels a stylised word
    QVERIFY(!realPersonCheck(u"Taylor Swift on stage", u"cartoon").refuse);
    QVERIFY(realPersonCheck(u"Taylor Swift on stage", u"photorealistic cartoon").refuse);
    QVERIFY(realPersonCheck(u"Mary Johnson portrait", u"candid photo").refuse);
    QVERIFY(!realPersonCheck(u"Mary Johnson portrait").refuse);       // a plain name without a photographic look
    // titles: places and "of" are not names; the reason names the trigger
    QVERIFY(!realPersonCheck(u"President Street at night, photo").refuse);
    QVERIFY(!realPersonCheck(u"the president of our rotary club, photo").refuse);
    const RealPersonVerdict v = realPersonCheck(u"Governor Smith signs a bill, press photo");
    QVERIFY(v.refuse);
    QVERIFY2(v.reason.contains(QStringLiteral("Governor Smith")), qPrintable(v.reason));
    QVERIFY(!v.names.isEmpty() && v.names.front().startsWith(QStringLiteral("Governor Smith")));
    QVERIFY(!v.stylized);
    QVERIFY(realPersonCheck(u"a deepfake of my boss").refuse);       // real-person wording
    QVERIFY(!realPersonCheck(u"a photo of a lighthouse at dawn").refuse);
}

void TestStudioSafety::pictureRuleLookalikeLetters() {
    // mcp-fix-media.ts S9: zero-width, Cyrillic / Greek look-alikes, soft hyphen, word joiner, full-width
    const QStringList cases{u"photorealistic photo of Barack O​bama at a rally"_s, u"photorealistic photo of Bаrack Obаma at a rally"_s,
                            u"photorealistic photo of Elon M­usk"_s,           u"press photo of Οbama waving"_s,
                            u"photo of Taylor⁠ Swift on stage"_s,              u"realistic photo of ＢＡＲＡＣＫ ＯＢＡＭＡ"_s};
    for (const QString& c : cases) QVERIFY2(realPersonCheck(c).refuse, qPrintable(c));
    QVERIFY(!realPersonCheck(u"кофейня на углу, фото").refuse);
}

void TestStudioSafety::parityWithTypeScript() {
    const QJsonArray cases = QJsonDocument::fromJson(QByteArray(kTsGolden)).array();
    QVERIFY(cases.size() >= 50);
    auto list = [](const QJsonValue& v) {
        QStringList out;
        for (const QJsonValue& x : v.toArray()) out << x.toString();
        return out;
    };
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString t = c.value(u"t").toString();
        const RealPersonVerdict r = realPersonCheck(t);
        QVERIFY2(r.refuse == c.value(u"rpc").toBool(), qPrintable(t));
        QVERIFY2(r.stylized == c.value(u"sty").toBool(), qPrintable(t));
        QVERIFY2(r.names == list(c.value(u"names")), qPrintable(t + QStringLiteral(" names: ") + r.names.join(u'|')));
        QVERIFY2(publicFiguresIn(t) == list(c.value(u"pf")), qPrintable(t + QStringLiteral(" pf: ") + publicFiguresIn(t).join(u'|')));
        QVERIFY2(publicFigureInName(t) == c.value(u"nm").toString(), qPrintable(t + QStringLiteral(" nm: ") + publicFigureInName(t)));
        const auto hit = realPeopleProblem(t);
        QVERIFY2((hit ? hit->reason : QString()) == c.value(u"spot").toString(), qPrintable(t + QStringLiteral(" spot: ") + (hit ? hit->reason : QString())));
    }
}

void TestStudioSafety::spotRules() {
    // mcp-spot.ts
    const QStringList yes{QStringLiteral("a photorealistic image of Taylor Swift drinking coffee"), QStringLiteral("Elon Musk holding our product"),
                          QStringLiteral("deepfake of my boss"),        QStringLiteral("a photo of the president waving"),
                          QStringLiteral("realistic celebrity endorsing the cafe"), QStringLiteral("King Charles at the counter")};
    const QStringList no{QStringLiteral("a King Charles spaniel on a sofa"), QStringLiteral("a barista in a green apron"),
                         QStringLiteral("climbing the rock at dawn"),         QStringLiteral("the president of our rotary club will speak"),
                         QStringLiteral("Main Street Coffee mug"),            QStringLiteral("a queen bee on a flower")};
    for (const QString& s : yes) QVERIFY2(realPeopleProblem(s).has_value(), qPrintable(s));
    for (const QString& s : no) QVERIFY2(!realPeopleProblem(s).has_value(), qPrintable(s + QStringLiteral(" → ") + realPeopleProblem(s).value_or(SafetyHit{}).reason));
    // mcp-fix-spot.ts S9-spot: zero-width / soft hyphen do not slip past the wording rules
    QVERIFY(realPeopleProblem(u"a deep​fake of my boss").has_value());
    QVERIFY(realPeopleProblem(u"a photo of the pre­sident waving").has_value());
    // Studio: look-alike letters in the wording too
    QVERIFY(realPeopleProblem(u"a dееpfake of my boss").has_value());
    // the rest of the wording rules, and the messages
    QVERIFY(realPeopleProblem(u"face swap my brother onto a pilot").has_value());
    QVERIFY(realPeopleProblem(u"footage of the current US senator in a diner").has_value());
    QVERIFY(realPeopleProblem(u"an actual person from our town, recognisable face").has_value());
    QVERIFY(realPeopleProblem(u"a famous actor, hyper-realistic").has_value());
    QVERIFY(realPeopleProblem(u"Obama waving").has_value());
    const auto hit = realPeopleProblem(u"A photorealistic spot where Taylor Swift drinks our coffee and smiles.");
    QVERIFY(hit);
    QCOMPARE(hit->match, QStringLiteral("taylor swift"));
    const QString msg = refusal(*hit, u"the brief");
    QVERIFY2(msg.startsWith(QStringLiteral("Refused: the brief names a real public figure (\"taylor swift\").")), qPrintable(msg));
    QVERIFY(msg.contains(realPeoplePolicy()));
    // the invented-people clause is added once
    const QString p = withInventedPeople(QStringLiteral("a barista pours latte art"));
    QVERIFY(p.endsWith(inventedPeopleClause()));
    QCOMPARE(withInventedPeople(p), p);
}

void TestStudioSafety::eachToolKeepsItsStrictestRule() {
    // mcp-integration.ts: spots refuse a caricature, pictures allow it
    QVERIFY(realPeopleProblem(u"a cartoon caricature of Taylor Swift").has_value());
    QVERIFY(!realPersonCheck(u"a cartoon caricature of Taylor Swift").refuse);
    QVERIFY(checkVisual(u"a cartoon caricature of Taylor Swift", {}, VisualRule::Spot).refuse);
    QVERIFY(!checkVisual(u"a cartoon caricature of Taylor Swift", {}, VisualRule::Picture).refuse);
    // the spot rule also carries the picture rule (a title + name): stricter than either alone
    QVERIFY(!realPeopleProblem(u"Senator Jane Doe at a podium").has_value());
    QVERIFY(checkVisual(u"Senator Jane Doe at a podium", {}, VisualRule::Spot).refuse);
    // names from the old voice / spot / picture lists reach every tool
    QVERIFY(realPeopleProblem(u"Abraham Lincoln at the counter").has_value());
    QVERIFY(realPeopleProblem(u"Kylie Jenner drinking coffee").has_value());
    QVERIFY(!publicFigureReason({QStringLiteral("Sydney Sweeney")}).isEmpty());
    QVERIFY(!publicFigureReason({}, {QStringLiteral("narration by kylie jenner")}).isEmpty());
    const Verdict ok = checkVisual(u"a barista in a green apron pours coffee, photo", u"cinematic");
    QVERIFY(!ok.refuse);
    QVERIFY(ok.code.isEmpty());
}

void TestStudioSafety::consentRecord() {
    // mcp-voice.ts
    QVERIFY(msgOf(checkConsent(std::nullopt, kNow)).contains(QStringLiteral("no consent record")));
    QCOMPARE(codeOf(checkConsent(std::nullopt, kNow)), QStringLiteral("no_consent"));
    ConsentInput c = janeConsent();
    c.date.clear();
    auto r = checkConsent(c, kNow);
    QVERIFY(msgOf(r).contains(QStringLiteral("missing date")));
    QCOMPARE(r->missing, QStringList{QStringLiteral("date")});
    c = janeConsent();
    c.statement = QStringLiteral("Here is a recording of my voice for you.");
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("statement must say")));
    QCOMPARE(codeOf(checkConsent(c, kNow)), QStringLiteral("weak_consent"));
    c = janeConsent();
    c.date = QStringLiteral("2099-01-01");
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("future")));
    c.date = QStringLiteral("2026-02-31");
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("YYYY-MM-DD")));
    c.date = QStringLiteral("2026-13-01");                            // Studio: no roll-over into January
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("YYYY-MM-DD")));
    c.date = QStringLiteral("1999-12-31");
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("implausibly old")));
    c.date = QStringLiteral("2026-10-27");                            // within 36 h of now (time zones)
    QVERIFY(!checkConsent(c, kNow));
    c = janeConsent();
    c.givenBy = QStringLiteral("Bob Smith");
    QVERIFY(msgOf(checkConsent(c, kNow)).contains(QStringLiteral("not by the speaker")));
    QCOMPARE(codeOf(checkConsent(c, kNow)), QStringLiteral("consent_not_from_speaker"));
    c = janeConsent();
    c.speakerName = QStringLiteral("Sam Doe");
    c.givenBy = QStringLiteral("Jane Doe, parent and legal guardian of Sam Doe");
    ConsentRecord rec;
    QVERIFY(!checkConsent(c, kNow, &rec));
    QVERIFY(!rec.selfGiven);
    QVERIFY(!checkConsent(janeConsent(), kNow, &rec));
    QVERIFY(rec.selfGiven);
    c = janeConsent();
    c.givenBy = QStringLiteral("myself");
    QVERIFY(!checkConsent(c, kNow, &rec) && rec.selfGiven);
    c.speakerName = QStringLiteral("J");
    QCOMPARE(codeOf(checkConsent(c, kNow)), QStringLiteral("incomplete_consent"));
    // the stored form round-trips
    const QJsonObject j = storedConsent();
    const ConsentRecord back = ConsentRecord::fromJson(j);
    QCOMPARE(back.speakerName, QStringLiteral("Jane Doe"));
    QVERIFY(back.confirmedByUser && back.selfGiven);
    QCOMPARE(back.toJson(), j);
}

void TestStudioSafety::consentPublicFigures() {
    // mcp-voice.ts
    QVERIFY(!publicFigureReason({QStringLiteral("Barack Obama")}).isEmpty());
    QVERIFY(!publicFigureReason({QStringLiteral("BEYONCÉ")}).isEmpty());
    QVERIFY(!publicFigureReason({QStringLiteral("Senator Jane Doe")}).isEmpty());
    QVERIFY(!publicFigureReason({}, {QStringLiteral("make it sound like a news anchor")}).isEmpty());
    QVERIFY(publicFigureReason({QStringLiteral("Jane Doe"), QStringLiteral("Jane narration")}, {janeConsent().statement}).isEmpty());
    // mcp-integration.ts: a surname matches in any case in a name field
    QVERIFY(!publicFigureReason({QStringLiteral("obama")}).isEmpty());
    // titles and imitation in names; free text checks full names only (a stage name in a statement is not refused)
    QVERIFY(publicFigureReason({QStringLiteral("Vice-President voice")}).contains(QStringLiteral("vice president")));
    QVERIFY(publicFigureReason({QStringLiteral("Jane impersonating someone")}).contains(QStringLiteral("imitation")));
    QVERIFY(publicFigureReason({}, {QStringLiteral("in the voice of my grandmother")}).contains(QStringLiteral("imitation")));
    QVERIFY(publicFigureReason({}, {QStringLiteral("I love Oprah's show and agree to the cloning")}).isEmpty());
    QVERIFY(publicFigureReason({}, {QStringLiteral("I am Barack Obama and I agree")}).contains(QStringLiteral("barack obama")));
    QVERIFY(publicFigureReason({QStringLiteral("x")}).isEmpty());
}

void TestStudioSafety::consentRecheckAtSendTime() {
    CloneUse use{QStringLiteral("Jane narration"), storedConsent(), QStringLiteral("Hello, this is my sample."),
                 QStringLiteral("Fresh coffee on Main Street."), false};
    QVERIFY(!checkCloneUse(use, kNow));
    // an agent never clones
    CloneUse u = use;
    u.fromAgent = true;
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("agent"));
    // a record a person never confirmed
    u = use;
    u.consent = storedConsent(false);
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("unconfirmed"));
    // a hand-edited row: statement weakened, consent removed, a public-figure speaker, a confirmed flag as a string
    u = use;
    u.consent[QStringLiteral("statement")] = QStringLiteral("sure");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("weak_consent"));
    u = use;
    u.consent = {};
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("no_consent"));
    u = use;
    u.consent[QStringLiteral("speaker_name")] = QStringLiteral("Taylor Swift");
    u.consent[QStringLiteral("given_by")] = QStringLiteral("Taylor Swift");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("public_figure"));
    u = use;
    u.consent[QStringLiteral("confirmed_by_user")] = QStringLiteral("true");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("unconfirmed"));
    // the text to speak and the transcript are read too
    u = use;
    u.text = QStringLiteral("Say this as if you were Barack Obama");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("public_figure"));
    u = use;
    u.text = QStringLiteral("do it in the voice of a famous rapper");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("public_figure"));
    u = use;
    u.profileName = QStringLiteral("Morgan Freeman narration");
    QCOMPARE(codeOf(checkCloneUse(u, kNow)), QStringLiteral("public_figure"));
}

void TestStudioSafety::failClosed() {
    // a data file that did not load: every check answers "real person"
    testing::setData(QJsonObject{});
    auto restore = qScopeGuard([] { testing::resetData(); });
    QString why;
    QVERIFY(!listsReady(&why));
    QVERIFY(!why.isEmpty());
    QVERIFY(realPersonCheck(u"a photo of a lighthouse at dawn").refuse);
    QVERIFY(looksLikePublicFigure(u"Jane Doe"));
    QVERIFY(!findPublicFigure(u"a barista").isEmpty());
    QVERIFY(realPeopleProblem(u"a barista in a green apron").has_value());
    QVERIFY(!publicFigureReason({QStringLiteral("Jane Doe")}).isEmpty());
    const Verdict v = checkVisual(u"a lighthouse", {}, VisualRule::Picture);
    QVERIFY(v.refuse);
    QCOMPARE(v.code, QStringLiteral("safety_data_unavailable"));
    const QJsonObject img{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a lighthouse")}};
    QVERIFY(checkOutgoing(u"/images", img).refuse);
    // a data file with an emptied list, and one whose pattern does not compile, fail closed too
    QJsonObject d = data::load(u"realpeople");
    d[QStringLiteral("publicFigures")] = QJsonArray{};
    testing::setData(d);
    QVERIFY(!listsReady());
    d = data::load(u"realpeople");
    QJsonObject pats = d.value(u"patterns").toObject();
    pats[QStringLiteral("stylized")] = QJsonObject{{QStringLiteral("source"), QStringLiteral("(unclosed")}, {QStringLiteral("flags"), QStringLiteral("i")}};
    d[QStringLiteral("patterns")] = pats;
    testing::setData(d);
    QVERIFY(!listsReady(&why));
    QVERIFY(realPersonCheck(u"a photo of a lighthouse at dawn").refuse);
    // the shipped data comes back
    testing::resetData();
    QVERIFY(listsReady());
    QVERIFY(!realPersonCheck(u"a photo of a lighthouse at dawn").refuse);
}

void TestStudioSafety::hookClassifiesRequests() {
    const QJsonObject prompt{{QStringLiteral("prompt"), QStringLiteral("x")}};
    QCOMPARE(classify(u"/images", prompt), RequestKind::Image);
    QCOMPARE(classify(u"/api/v1/images/generations", prompt), RequestKind::Image);
    QCOMPARE(classify(u"https://openrouter.ai/api/v1/images/edits?x=1", prompt), RequestKind::Image);
    QCOMPARE(classify(u"/api/v1/videos", prompt), RequestKind::Video);
    QCOMPARE(classify(u"/api/v1/videos/job-1", prompt), RequestKind::Other);          // a poll
    QCOMPARE(classify(u"/api/v1/videos/job-1/content", prompt), RequestKind::Other);
    QCOMPARE(classify(u"/chat/completions", QJsonObject{}), RequestKind::Other);
    QCOMPARE(classify(u"/chat/completions", QJsonObject{{QStringLiteral("modalities"), QJsonArray{QStringLiteral("text"), QStringLiteral("image")}}}),
             RequestKind::Image);
    QCOMPARE(classify(u"/audio/speech", QJsonObject{{QStringLiteral("voice"), QStringLiteral("alloy")}}), RequestKind::Other);
    const QJsonObject clone{{QStringLiteral("input"), QStringLiteral("hi")},
                            {QStringLiteral("input_references"),
                             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("input_audio")},
                                                    {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), QStringLiteral("data:audio/wav;base64,AAAA")}}}}}}};
    QCOMPARE(classify(u"/api/v1/audio/speech", clone), RequestKind::CloneSpeech);
    QCOMPARE(classify(u"/embeddings", prompt), RequestKind::Other);
    // the prompt as a model reads it: prompt + every text part of the messages
    const QJsonObject chat{{QStringLiteral("messages"),
                            QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), QStringLiteral("You draw.")}},
                                       QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                   {QStringLiteral("content"),
                                                    QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("Elon Musk")}},
                                                               QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")}}}}}}}};
    QCOMPARE(promptText(chat), QStringLiteral("You draw.\nElon Musk"));
    // negative_prompt is what NOT to draw: not part of the prompt
    QCOMPARE(promptText(QJsonObject{{QStringLiteral("prompt"), QStringLiteral("a lighthouse")}, {QStringLiteral("negative_prompt"), QStringLiteral("real people")}}),
             QStringLiteral("a lighthouse"));
    // /responses: message items with input_text parts are read, and an image_generation tool makes it a picture request
    const QJsonObject resp{{QStringLiteral("tools"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("image_generation")}}}},
                           {QStringLiteral("input"),
                            QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                   {QStringLiteral("content"),
                                                    QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("input_text")},
                                                                           {QStringLiteral("text"), QStringLiteral("a photo of Elon Musk")}}}}}}}};
    QCOMPARE(classify(u"/responses", resp), RequestKind::Image);
    QCOMPARE(promptText(resp), QStringLiteral("a photo of Elon Musk"));
    QVERIFY(checkOutgoing(u"/api/v1/responses", resp).refuse);
}

void TestStudioSafety::hookReadsStyleBansAsBans() {
    // the negation neutraliser: bans lose their "real/identifiable" adjectives, requests keep them
    QCOMPARE(neutraliseNegatedPeople(u"realistic b-roll, no identifiable people"), QStringLiteral("realistic b-roll, no people"));
    QCOMPARE(neutraliseNegatedPeople(u"Avoid: staring into the lens, text, identifiable real people."), QStringLiteral("Avoid: staring into the lens, text, people."));
    QCOMPARE(neutraliseNegatedPeople(u"Do not draw identifiable real people."), QStringLiteral("Do not draw people."));
    QCOMPARE(neutraliseNegatedPeople(u"a photo of real people at a rally"), QStringLiteral("a photo of real people at a rally"));
    for (const auto* req : {u"photo of real people at a rally. Avoid: blur.", u"Avoid: blur. A photo of identifiable people at a rally.",
                            u"not only real people but actual celebrities, photo", u"no filter, recognizable politicians at a dinner",
                            u"Avoid: blur, a deepfake of the mayor.", u"Avoid: blur, a photo of the president.", u"Avoid: blur, Barack Obama, text."}) {
        const QJsonObject body{{QStringLiteral("prompt"), QString::fromUtf16(req)}};
        QVERIFY2(checkOutgoing(u"/images", body).refuse, qPrintable(QString::fromUtf16(req)));
    }

    // every shipped style, composed as plan.ts imagePrompt / videoPrompt compose a spot shot, passes under both rules;
    // with the Avoid list inline, in negative_prompt, and with the news formats' guard sentence appended
    QString err;
    const QJsonObject styles = data::load(u"styles", &err);
    QVERIFY2(!styles.isEmpty(), qPrintable(err));
    const QString scene = QStringLiteral("A friendly barista in her 30s pours latte art at a small cafe counter.");
    SendContext picture;
    picture.rule = VisualRule::Picture;
    int n = 0;
    for (const QJsonValue& sv : styles.value(u"styles").toArray()) {
        const QJsonObject s = sv.toObject();
        const QString visual = s.value(u"visual").toString(), avoid = s.value(u"avoid").toString(), camera = s.value(u"camera").toString();
        const QString still = QStringList{scene, camera + u'.', u"Style: "_s + visual + u'.', u"vertical 9:16 frame, subject centred with headroom."_s,
                                          inventedPeopleClause()}.join(u' ');
        const QString image = still + u" Avoid: "_s + avoid + u". No text, letters, captions, logos or watermarks."_s;
        const QString video = QStringList{scene, u"Camera: "_s + camera + u'.', u"Style: "_s + visual + u'.',
                                          u"Smooth, natural motion; one continuous shot."_s, inventedPeopleClause(), u"No on-screen text or logos."_s}.join(u' ');
        const QList<std::pair<QString, QJsonObject>> bodies{
            {u"/images"_s, QJsonObject{{u"prompt"_s, image}}},
            {u"/images"_s, QJsonObject{{u"prompt"_s, still}, {u"negative_prompt"_s, avoid}}},
            {u"/images"_s, QJsonObject{{u"prompt"_s, image + u" Do not draw identifiable real people."_s}}},
            {u"/videos"_s, QJsonObject{{u"prompt"_s, video}}},
        };
        for (const auto& [path, body] : bodies)
            for (SendContext ctx : {SendContext{}, picture}) {
                ctx.style = visual;
                const Verdict v = checkOutgoing(path, body, ctx);
                QVERIFY2(!v.refuse, qPrintable(s.value(u"id").toString() + u": "_s + v.reason + u"\n  "_s + body.value(u"prompt").toString()));
                ++n;
            }
    }
    QVERIFY(n > 100);
}

void TestStudioSafety::hookBlocksBeforeSend() {
    test::MockOpenRouter mock;
    QVERIFY2(mock.start(), qPrintable(mock.errorString()));
    QNetworkAccessManager nam;
    SendContext picture;
    picture.rule = VisualRule::Picture;

    // a public figure in an image body: never reaches the wire
    Verdict v;
    const QJsonObject bad{{QStringLiteral("model"), QStringLiteral("acme/img-1")},
                          {QStringLiteral("prompt"), QStringLiteral("photorealistic photo of Bаrack Obаma at a rally")}};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/images"), bad, picture, &v), -1);
    QVERIFY(v.refuse);
    QCOMPARE(v.code, QStringLiteral("real_person"));
    QVERIFY(v.names.contains(QStringLiteral("barack obama")));
    // a video submit under the spot rule, even as a caricature
    const QJsonObject vid{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), QStringLiteral("a cartoon caricature of Taylor Swift dancing")}};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/videos"), vid, SendContext{}, &v), -1);
    // image output asked for through chat
    const QJsonObject chatImg{{QStringLiteral("model"), QStringLiteral("acme/chat-img")},
                              {QStringLiteral("modalities"), QJsonArray{QStringLiteral("image"), QStringLiteral("text")}},
                              {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                                                  {QStringLiteral("content"), QStringLiteral("a selfie of Elon Musk")}}}}};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/chat/completions"), chatImg, picture, &v), -1);
    // a cloned voice without its profile, and one whose stored consent was never confirmed
    const QJsonObject clone{{QStringLiteral("model"), QStringLiteral("acme/tts-clone")}, {QStringLiteral("input"), QStringLiteral("Fresh coffee on Main Street.")},
                            {QStringLiteral("input_references"),
                             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("input_audio")},
                                                    {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), QStringLiteral("data:audio/wav;base64,UklGRg==")}}}},
                                        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("Hello, my sample.")}}}}};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/audio/speech"), clone, SendContext{}, &v), -1);
    QCOMPARE(v.code, QStringLiteral("voice_consent:no_consent"));
    SendContext voice;
    voice.nowMs = kNow;
    voice.voice = CloneUse{QStringLiteral("Jane narration"), storedConsent(false), {}, {}, false};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/audio/speech"), clone, voice, &v), -1);
    QCOMPARE(v.code, QStringLiteral("voice_consent:unconfirmed"));
    // the text in the body is what is checked (not a caller-provided copy)
    QJsonObject cloneObama = clone;
    cloneObama[QStringLiteral("input")] = QStringLiteral("Hi, I'm Barack Obama and I approve this message.");
    voice.voice->consent = storedConsent(true);
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/audio/speech"), cloneObama, voice, &v), -1);
    QCOMPARE(v.code, QStringLiteral("voice_consent:public_figure"));
    // ... also when the caller passes its own (benign) copy of the text
    SendContext voiceWithText = voice;
    voiceWithText.voice->text = QStringLiteral("Fresh coffee on Main Street.");
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/audio/speech"), cloneObama, voiceWithText, &v), -1);
    QCOMPARE(v.code, QStringLiteral("voice_consent:public_figure"));
    QCOMPARE(mock.count(), 0);   // nothing above left the process

    // allowed requests go out
    const QJsonObject good{{QStringLiteral("model"), QStringLiteral("acme/img-1")},
                           {QStringLiteral("prompt"), withInventedPeople(QStringLiteral("a friendly barista in her 30s pours latte art, photo"))}};
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/images"), good, SendContext{}, &v), 200);
    QVERIFY(!v.refuse);
    QCOMPARE(guardedPost(nam, mock, QStringLiteral("/audio/speech"), clone, voice, &v), 200);
    QVERIFY(!v.refuse);
    QCOMPARE(mock.count(QStringLiteral("/images")), 1);
    QCOMPARE(mock.count(QStringLiteral("/audio/speech")), 1);
    // text chat is not a picture request: a name in a news summary passes
    QVERIFY(!checkOutgoing(u"/chat/completions", QJsonObject{{QStringLiteral("messages"), QJsonArray{QJsonObject{
                                                                  {QStringLiteral("role"), QStringLiteral("user")},
                                                                  {QStringLiteral("content"), QStringLiteral("Summarise what Barack Obama said today.")}}}}})
                 .refuse);
    // the refusal was logged with the path and reason, never the body
    QVERIFY(log::flush());
    const QString recent = log::recent(200).join(QLatin1Char('\n'));
    QVERIFY2(recent.contains(QStringLiteral("blocked before send: image /images (real_person")), qPrintable(recent.right(2000)));
    QVERIFY(!recent.contains(QStringLiteral("UklGRg==")));
    mock.stop();
}

void TestStudioSafety::redaction() {
    // mcp-core.ts redactValue
    const QJsonObject in{{QStringLiteral("apiKey"), QStringLiteral("abc12345")},
                         {QStringLiteral("approve"), QStringLiteral("apv_1234567890abcdef1234")},
                         {QStringLiteral("max_tokens"), 50},
                         {QStringLiteral("keywords"), QJsonArray{QStringLiteral("a")}},
                         {QStringLiteral("deep"), QJsonObject{{QStringLiteral("password"), QStringLiteral("x1234")}}},
                         {QStringLiteral("s"), QStringLiteral("Authorization: Bearer abcdefghijklmnop1234")},
                         {QStringLiteral("keyframes"), QStringLiteral("kept")},
                         {QStringLiteral("token"), QStringLiteral("")},
                         {QStringLiteral("secret"), true}};
    const QJsonObject v = redactObject(in);
    QCOMPARE(v.value(u"apiKey").toString(), QStringLiteral("[redacted]"));
    QCOMPARE(v.value(u"approve").toString(), QStringLiteral("[redacted]"));
    QCOMPARE(v.value(u"max_tokens").toInt(), 50);
    QVERIFY(v.value(u"keywords").isArray());
    QCOMPARE(v.value(u"deep").toObject().value(u"password").toString(), QStringLiteral("[redacted]"));
    QVERIFY(!v.value(u"s").toString().contains(QStringLiteral("abcdefghijklmnop1234")));
    QCOMPARE(v.value(u"keyframes").toString(), QStringLiteral("kept"));
    QCOMPARE(v.value(u"token").toString(), QString());       // empty and boolean values are kept as they are
    QCOMPARE(v.value(u"secret").toBool(), true);
    QCOMPARE(redactBinary(QByteArray(10, 'x')), QStringLiteral("[binary 10 bytes]"));
    // key shapes in free text
    const QString t = redactText(QStringLiteral("key sk-or-v1-0123456789abcdef0123456789abcdef end; api_key=hunter2xyz; "
                                                "ghp_abcdefghijklmnopqrstuvwxyz0123; AKIAABCDEFGHIJKLMNOP; eyJhbGciOi.eyJzdWIiOi.sig"),
                                 {QStringLiteral("my-extra-secret-value")});
    for (const char* s : {"0123456789abcdef0123", "hunter2xyz", "ghp_abcdefghijklmnop", "AKIAABCDEFGHIJKLMNOP", "eyJzdWIiOi"})
        QVERIFY2(!t.contains(QLatin1String(s)), qPrintable(t));
    QVERIFY(!redactText(QStringLiteral("x my-extra-secret-value y"), {QStringLiteral("my-extra-secret-value")}).contains(QStringLiteral("extra")));
    QCOMPARE(redactText(QStringLiteral("hello /videos/abc?x=1")), QStringLiteral("hello /videos/abc?x=1"));
    // data URIs collapse; long strings keep a head and a hash
    const QJsonObject body{{QStringLiteral("image"), QStringLiteral("data:image/png;base64,") + QString(4000, QLatin1Char('A'))}};
    QVERIFY(redactObject(body).value(u"image").toString().startsWith(QStringLiteral("[data:image/png removed")));
    const QString longText = QString(2500, QLatin1Char('w'));
    const QString cut = truncate(longText, 2000);
    QVERIFY(cut.startsWith(QString(2000, QLatin1Char('w')) + QStringLiteral("…[truncated 500 chars, sha256:")));
    QCOMPARE(cut.size(), 2000 + QStringLiteral("…[truncated 500 chars, sha256:123456789012]").size());
    QCOMPARE(truncate(QStringLiteral("short"), 2000), QStringLiteral("short"));
    // structure limits
    QJsonArray many;
    for (int i = 0; i < 60; ++i) many.append(i);
    const QJsonArray ra = redactValue(many).toArray();
    QCOMPARE(ra.size(), 51);
    QCOMPARE(ra.last().toString(), QStringLiteral("[10 more items]"));
    QJsonValue deep = QStringLiteral("bottom");
    for (int i = 0; i < 9; ++i) deep = QJsonObject{{QStringLiteral("n"), deep}};
    QVERIFY(QJsonDocument(redactValue(deep).toObject()).toJson().contains("[nested too deep]"));
    // headers
    const auto h = redactHeaders({{"Authorization", "Bearer sk-or-v1-0123456789abcdef0123"}, {"X-Api-Key", "k-1234567890"}, {"Accept", "application/json"}});
    QCOMPARE(h.at(0).second, QByteArray("[redacted]"));
    QCOMPARE(h.at(1).second, QByteArray("[redacted]"));
    QCOMPARE(h.at(2).second, QByteArray("application/json"));
    // forLog: a request body as one line
    const QString line = forLog(QJsonObject{{QStringLiteral("model"), QStringLiteral("m")}, {QStringLiteral("api_key"), QStringLiteral("sk-or-v1-0123456789abcdef0123")}});
    QVERIFY2(line.contains(QStringLiteral("\"api_key\":\"[redacted]\"")) && !line.contains(QStringLiteral("0123456789abcdef")), qPrintable(line));
}

void TestStudioSafety::logsNeverCarrySecrets() {
    const QString key = QStringLiteral("sk-or-v1-") + QString(40, QLatin1Char('f'));
    const QString registered = QStringLiteral("plain-registered-secret-42");
    log::addSecret(registered);
    log::warn(u"safety-test", QStringLiteral("calling with key %1 and header Authorization: Bearer abc.DEF-123456789xyz and %2").arg(key, registered));
    log::info(u"safety-test", QStringLiteral("body %1").arg(forLog(QJsonObject{{QStringLiteral("token"), QStringLiteral("tok-not-shaped-like-a-key")}})));
    QVERIFY(log::flush());
    QFile f(log::currentFile());
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(log::currentFile()));
    const QString text = QString::fromUtf8(f.readAll());
    QVERIFY(text.contains(QStringLiteral("safety-test")));
    QVERIFY(!text.contains(key));
    QVERIFY(!text.contains(QStringLiteral("ffffffffffffffff")));
    QVERIFY(!text.contains(QStringLiteral("abc.DEF-123456789xyz")));
    QVERIFY(!text.contains(registered));
    QVERIFY(!text.contains(QStringLiteral("tok-not-shaped-like-a-key")));
    QVERIFY(text.contains(QStringLiteral("[redacted]")));
}

QTEST_GUILESS_MAIN(TestStudioSafety)
#include "test_studio_safety.moc"
