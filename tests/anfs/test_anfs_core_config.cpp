// test_anfs_core_config - ANFS-F1b: anfs.json (core/config), the process log (core/log), the service registry
// (core/svc), ids/digests/encodings (core/ids) and the injectable clock (core/clock).
//
// - Config: every Config.cs key round-trips (file -> Config -> file, and Config -> file -> Config) with unknown keys
//   kept in place (top level, inside wan, folders and tls); the absent-when-default rules; the exact pretty file text;
//   validation texts; toSafeJval; gate rules and password records (ports of the C# SelfTestCore Config_* cases).
// - ConfigStore: load/save/update semantics, listeners, a rewrite with the same size and mtime caught by the SHA-256,
//   and atomic saves while raw-file readers (sharing and non-sharing), snapshot readers and 4 concurrent updaters run.
// - ConfigWatcher on a ManualClock: nothing at 1.98 s, the change at exactly 2.00 s, one report per bad file.
// - Log: line format, redaction vectors (the C# Log.Redact regexes plus auth/key), secrets, midnight rotation with 14
//   files kept, the queue invariant under a 200,000-line burst, the stdout rate limit, and callers that keep running
//   while stdout is a pipe nobody reads.
// - ids: digest/HMAC vectors (RFC 1321/3174/4231, FIPS 180), id formats by regex, hex/base64, constant-time compare.
#include "core/clock.h"
#include "core/config.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "core/svc.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

using namespace anfs;

namespace {

QByteArray qb(const std::string& s) { return QByteArray(s.data(), static_cast<qsizetype>(s.size())); }
#define SEQ(actual, expected) QCOMPARE(qb(actual), qb(std::string(expected)))

std::string readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

std::wstring wpath(const QString& p) { return QDir::toNativeSeparators(p).toStdWString(); }

int64_t localEpochMs(int y, int mo, int d, int h, int mi, int s, int ms)
{
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tm)) * 1000 + ms;
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos) j = text.size();
        std::string l = text.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
        i = j + 1;
    }
    return out;
}

int countContaining(const std::vector<std::string>& ls, const std::string& needle)
{
    int n = 0;
    for (const std::string& l : ls)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

QStringList tmpFiles(const QString& dir) { return QDir(dir).entryList({"*.tmp"}, QDir::Files); }

template <class Pred> bool waitReal(Pred p, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!p()) {
        if (t.elapsed() > ms) return p();
        QThread::msleep(5);
    }
    return true;
}

// Every typed field plus the written form: two configs that compare equal here write the same anfs.json.
QString diffConfig(const Config& a, const Config& b)
{
    auto pr = [](const std::optional<PasswordRecord>& r) {
        return r ? QString::fromStdString(json::stringify(r->toJval())) : QString("none");
    };
    auto rules = [](const std::vector<GateRule>& v) {
        QStringList l;
        for (const GateRule& r : v) l << QString::fromStdString(r.toString());
        return l.join('|');
    };
    auto folders = [](const Config& c) {
        QStringList l;
        for (const auto& kv : c.folders) l << QString::fromStdString(kv.first + "=" + kv.second);
        return l.join('|');
    };
    auto list = [](const std::vector<std::string>& v) {
        QStringList l;
        for (const std::string& s : v) l << QString::fromStdString(s);
        return l.join('|');
    };
    QStringList d;
    if (a.mode != b.mode) d << "mode";
    if (a.nodeId != b.nodeId) d << "nodeId";
    if (a.name != b.name) d << "name";
    if (a.port != b.port) d << "port";
    if (a.tlsPort != b.tlsPort) d << "tlsPort";
    if (a.statusPort != b.statusPort) d << "statusPort";
    if (a.bindAll != b.bindAll) d << "bindAll";
    if (a.bindAddress != b.bindAddress) d << "bindAddress";
    if (a.test != b.test) d << "test";
    if (list(a.bridgeUrls) != list(b.bridgeUrls)) d << "bridgeUrls";
    if (a.shares != b.shares) d << "shares";
    if (a.exposeFullDrives != b.exposeFullDrives) d << "exposeFullDrives";
    if (a.allowWrite != b.allowWrite) d << "allowWrite";
    if (folders(a) != folders(b)) d << "folders";
    if (pr(a.wan.passwordRecord) != pr(b.wan.passwordRecord)) d << "wan.passwordRecord";
    if (rules(a.wan.gatedPaths) != rules(b.wan.gatedPaths)) d << "wan.gatedPaths";
    if (list(a.wan.trustedProxies) != list(b.wan.trustedProxies)) d << "wan.trustedProxies";
    if (a.wan.requireTls != b.wan.requireTls) d << "wan.requireTls";
    if (a.agentToken != b.agentToken) d << "agentToken";
    if (a.tls.pfx != b.tls.pfx) d << "tls.pfx";
    if (a.tls.passwordEnv != b.tls.passwordEnv) d << "tls.passwordEnv";
    if (list(a.corsAllow) != list(b.corsAllow)) d << "corsAllow";
    if (a.rev != b.rev) d << "rev";
    if (json::stringify(a.toJval()) != json::stringify(b.toJval())) d << "toJval";
    return d.join(',');
}

// Every key Config.cs knows (plus rev/statusPort), unknown keys at the top level and inside wan/folders/tls, keys in
// an order that is not the writer's own, and values that need the C# leniency (mode case, a padded URL, shares that
// need filling or skipping, non-string folders, unparsable gate rules).
const char* const kFullConfig =
    "{\"future\":{\"a\":[1,2]},\"mode\":\" Server \",\"nodeId\":\"n1\",\"name\":\"Studio\",\"port\":9000,\"tlsPort\":9443,"
    "\"statusPort\":8198,\"bindAll\":false,\"bindAddress\":\"10.0.0.5\",\"test\":true,\"exposeFullDrives\":false,"
    "\"allowWrite\":false,\"agentToken\":\"tok-123\",\"bridgeUrl\":[\"http://a:1\",\" http://b:2 \",5],"
    "\"bridgeFallbacks\":[\"http://c:3\",\"http://a:1\"],\"corsAllow\":[\"https://x.example\"],"
    "\"shares\":[{\"id\":\"s1\",\"name\":\"Media\",\"path\":\"D:\\\\media\"},{\"name\":\"OnlyName\",\"path\":\"E:\\\\\"},"
    "{\"id\":\"\",\"name\":\"\"},5],"
    "\"folders\":{\"dropbox\":\"D:\\\\drop\",\"Music\":\"M:\\\\\",\"num\":5},"
    "\"wan\":{\"passwordRecord\":{\"algo\":\"pbkdf2-sha256\",\"iterations\":600000,\"salt\":\"c2FsdA==\",\"hash\":\"aGFzaA==\"},"
    "\"gatedPaths\":[\"POST /api/upload/{id}\",\"/api/x\",{\"method\":\"get, head\",\"pattern\":\"api/y/*\"},\"* /api/z\",7,\"\"],"
    "\"trustedProxies\":[\"10.0.0.1\"],\"requireTls\":true,\"extraWan\":42},"
    "\"tls\":{\"pfx\":\"c.pfx\",\"passwordEnv\":\"PFXPW\",\"keep\":1},\"rev\":7,\"zzz\":null}";

const char* const kFullConfigWritten =
    "{\"future\":{\"a\":[1,2]},\"mode\":\"server\",\"nodeId\":\"n1\",\"name\":\"Studio\",\"port\":9000,\"tlsPort\":9443,"
    "\"statusPort\":8198,\"bindAll\":false,\"bindAddress\":\"10.0.0.5\",\"test\":true,\"exposeFullDrives\":false,"
    "\"allowWrite\":false,\"agentToken\":\"tok-123\",\"bridgeUrl\":\"http://a:1\","
    "\"bridgeFallbacks\":[\"http://b:2\",\"http://c:3\"],\"corsAllow\":[\"https://x.example\"],"
    "\"shares\":[{\"id\":\"s1\",\"name\":\"Media\",\"path\":\"D:\\\\media\"},{\"id\":\"OnlyName\",\"name\":\"OnlyName\",\"path\":\"E:\\\\\"}],"
    "\"folders\":{\"dropbox\":\"D:\\\\drop\",\"Music\":\"M:\\\\\",\"num\":5},"
    "\"wan\":{\"passwordRecord\":{\"algo\":\"pbkdf2-sha256\",\"iterations\":600000,\"salt\":\"c2FsdA==\",\"hash\":\"aGFzaA==\"},"
    "\"gatedPaths\":[\"POST /api/upload/{id}\",\"* /api/x\",\"GET,HEAD /api/y/*\",\"* /api/z\"],"
    "\"trustedProxies\":[\"10.0.0.1\"],\"requireTls\":true,\"extraWan\":42},"
    "\"tls\":{\"pfx\":\"c.pfx\",\"passwordEnv\":\"PFXPW\",\"keep\":1},\"rev\":7,\"zzz\":null}";

const char* const kDefaultsWritten =
    "{\"mode\":\"off\",\"nodeId\":\"\",\"name\":\"\",\"bindAll\":true,\"exposeFullDrives\":true,\"allowWrite\":true,"
    "\"corsAllow\":[],\"shares\":[],\"folders\":{},"
    "\"wan\":{\"gatedPaths\":[],\"trustedProxies\":[\"127.0.0.1\",\"::1\"],\"requireTls\":false}}";

// JSON.stringify(v, null, 2) + CRLF: also the C# Json.Serialize(d, true) form, so both binaries write one text.
const char* const kDefaultsFileText =
    "{\n  \"mode\": \"off\",\n  \"nodeId\": \"\",\n  \"name\": \"\",\n  \"bindAll\": true,\n  \"exposeFullDrives\": true,\n"
    "  \"allowWrite\": true,\n  \"corsAllow\": [],\n  \"shares\": [],\n  \"folders\": {},\n  \"wan\": {\n"
    "    \"gatedPaths\": [],\n    \"trustedProxies\": [\n      \"127.0.0.1\",\n      \"::1\"\n    ],\n"
    "    \"requireTls\": false\n  }\n}\r\n";

struct RedactCase {
    const char* in;
    const char* out;
};

// The first group equals Python re (the C# Log.Redact patterns); the second group is the C++ additions.
const RedactCase kRedact[] = {
    {"x-agent-auth: S3cretValue", "x-agent-auth: [redacted]"},
    {"Authorization: Bearer abc.def-123", "Authorization: [redacted]"},
    {"proxy-authorization=Basic Zm9vOmJhcg==", "proxy-authorization=[redacted]"},
    {"using bearer abcdefgh now", "using bearer [redacted] now"},
    {"Basic abc", "Basic abc"},
    {"{\"agentToken\":\"tok-123\",\"name\":\"x\"}", "{\"agentToken\":\"[redacted]\",\"name\":\"x\"}"},
    {"password=hunter2;user=bob", "password=\"[redacted]\";user=bob"},
    {"api_key: xyz, other", "api_key: \"[redacted]\", other"},
    {"token : \"spaced value\" end", "token : \"[redacted]\" end"},
    {"Cookie: sid=1\r\nnext line", "Cookie: [redacted]\r\nnext line"},
    {"the tokens: 5", "the tokens: 5"},
    {"secret=\"unterminated", "secret=\"unterminated"},
    {"AUTHORIZATION:abc", "AUTHORIZATION:[redacted]"},
    {"myAuthorization: x", "myAuthorization: x"},
    {"pwd= x", "pwd= \"[redacted]\""},
    {"url=http://h/?apikey=K1&b=2", "url=http://h/?apikey=\"[redacted]\"&b=2"},
    {"set-cookie=a=b; Path=/\nok", "set-cookie=[redacted]\nok"},
    {"lastTokenAt=5 passwordRecord={x}", "lastTokenAt=5 passwordRecord={x}"},
    {"\"password\" : \"p w\"", "\"password\" : \"[redacted]\""},
    {"Bearer\tabcdef123456 tail", "Bearer [redacted] tail"},
    {"api-key=1,api_key=2,apikey=3", "api-key=\"[redacted]\",api_key=\"[redacted]\",apikey=\"[redacted]\""},
    // C++: auth and key values; an already masked value is not quoted again (C# gives "X-Api-Key:\t\"[redacted]\"").
    {"GET /api/browse?auth=abc123&path=C%3A", "GET /api/browse?auth=\"[redacted]\"&path=C%3A"},
    {"{\"auth\":\"t1\",\"peer_key\":\"pk\",\"key\":\"k\"}", "{\"auth\":\"[redacted]\",\"peer_key\":\"[redacted]\",\"key\":\"[redacted]\"}"},
    {"stream-key=live_123 monkey=3 hotkey=F5", "stream-key=\"[redacted]\" monkey=3 hotkey=F5"},
    {"X-Api-Key:\tk-1", "X-Api-Key:\t[redacted]"},
    {"author=Ann authority: x", "author=Ann authority: x"},
    {"", ""},
};

} // namespace

class TestAnfsCoreConfig : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    // config
    void configRoundTripsEveryKey();
    void configDefaultsAndAbsentKeys();
    void configValidation();
    void configSafeAndRaw();
    void gateRulesAndPasswordRecord();
    void writeFileAtomicBasics();
    void storeLoadSaveUpdate();
    void storeListeners();
    void storeSeesSameSizeSameMtimeRewrite();
    void storeAtomicUnderConcurrentReaders();
    void watcherSeesChangeWithinTwoSecondsOnInjectedClock();
    void watcherStopsPromptlyOnSystemClock();

    // log
    void logRedactVectors();
    void logFileFormatSecretsAndCounters();
    void logRotatesAtMidnightAndKeepsFourteen();
    void logBurstNeverLosesErrorsAndAccountsForDrops();
    void logConsoleRateLimit();
    void logCallersNeverBlockOnStalledStdout();

    // svc, ids, clock
    void svcRegistry();
    void idsDigestVectors();
    void idsFormatsAndRandom();
    void hexBase64AndConstantTime();
    void clockSystemAndManual();
};

void TestAnfsCoreConfig::initTestCase() { setClock(nullptr); }

void TestAnfsCoreConfig::cleanup()
{
    setClock(nullptr);   // a failed slot must not leave its ManualClock installed
    Svc::clear();
}

// ---- config -------------------------------------------------------------------------------------------------------------

void TestAnfsCoreConfig::configRoundTripsEveryKey()
{
    const Config c = Config::fromJson(kFullConfig);
    QCOMPARE(QString::fromStdString(c.mode), QString("server"));
    QCOMPARE(QString::fromStdString(c.nodeId), QString("n1"));
    QCOMPARE(QString::fromStdString(c.name), QString("Studio"));
    QCOMPARE(c.port, 9000);
    QCOMPARE(c.tlsPort, 9443);
    QCOMPARE(c.statusPort, 8198);
    QCOMPARE(c.effectivePort(), 9000);
    QCOMPARE(c.effectiveTlsPort(), 9443);
    QVERIFY(!c.bindAll);
    QCOMPARE(QString::fromStdString(c.bindAddress), QString("10.0.0.5"));
    QVERIFY(c.test);
    QVERIFY(!c.exposeFullDrives);
    QVERIFY(!c.allowWrite);
    QCOMPARE(QString::fromStdString(c.agentToken), QString("tok-123"));
    // bridgeUrl (trimmed, non-strings skipped), then the fallbacks not already listed.
    QCOMPARE(c.bridgeUrls.size(), size_t(3));
    QCOMPARE(QString::fromStdString(c.bridgeUrl()), QString("http://a:1"));
    QCOMPARE(QString::fromStdString(c.bridgeUrls[1]), QString("http://b:2"));
    QCOMPARE(QString::fromStdString(c.bridgeUrls[2]), QString("http://c:3"));
    QCOMPARE(c.corsAllow.size(), size_t(1));
    QCOMPARE(c.shares.size(), size_t(2));
    QVERIFY((c.shares[0] == Share{"s1", "Media", "D:\\media"}));
    QVERIFY((c.shares[1] == Share{"OnlyName", "OnlyName", "E:\\"}));
    QCOMPARE(QString::fromStdString(c.dropbox()), QString("D:\\drop"));
    QCOMPARE(QString::fromStdString(c.folder("MUSIC")), QString("M:\\"));
    QCOMPARE(c.folders.size(), size_t(2));   // "num":5 is not a folder (stays in raw)
    QVERIFY(c.wan.passwordRecord && c.wan.passwordRecord->isSet());
    QCOMPARE(c.wan.passwordRecord->iterations, int64_t(600000));
    QCOMPARE(c.wan.gatedPaths.size(), size_t(4));
    QCOMPARE(QString::fromStdString(c.wan.gatedPaths[0].method), QString("POST"));
    QCOMPARE(QString::fromStdString(c.wan.gatedPaths[1].method), QString("*"));
    QCOMPARE(QString::fromStdString(c.wan.gatedPaths[2].toString()), QString("GET,HEAD /api/y/*"));   // C#: "GET, HEAD ..."
    QVERIFY(c.wan.gatedPaths[2].matches("head", "/api/y/z"));
    QCOMPARE(c.wan.trustedProxies.size(), size_t(1));
    QVERIFY(c.wan.requireTls);
    QCOMPARE(QString::fromStdString(c.tls.pfx), QString("c.pfx"));
    QCOMPARE(QString::fromStdString(c.tls.passwordEnv), QString("PFXPW"));
    QCOMPARE(c.rev, int64_t(7));

    // File -> Config -> file: typed keys replaced in place, unknown keys (future, extraWan, num, keep, zzz) kept where
    // they were.
    const std::string expected = kFullConfigWritten;
    SEQ(json::stringify(c.toJval()), expected);

    // ... and stable: the written text reads back to the same Config and writes the same bytes again.
    const Config again = Config::fromJson(c.toJsonText());
    QCOMPARE(diffConfig(c, again), QString());
    SEQ(json::stringify(again.toJval()), expected);
    SEQ(again.toJsonText(), c.toJsonText());

    // Config -> file -> Config for a config built in code (every typed field set to a non-default value).
    Config b;
    b.mode = "peer";
    b.nodeId = "node-7";
    b.name = "Edge \xC3\xA9";
    b.port = 18899;
    b.tlsPort = 18443;
    b.statusPort = 18198;
    b.bindAll = false;
    b.bindAddress = "192.168.1.9";
    b.test = true;
    b.bridgeUrls = {"http://127.0.0.1:18080", "http://127.0.0.1:18081"};
    b.shares = {{"share_1", "Media", "D:\\Media"}, {"w", "Web", "C:\\webroot"}};
    b.exposeFullDrives = false;
    b.allowWrite = false;
    b.setFolder("dropbox", "C:\\filedrop");
    b.setFolder("Drop2", "C:\\d2");
    PasswordRecord pr;
    pr.algo = "pbkdf2-sha256";
    pr.iterations = 1000;
    pr.salt = "c2FsdA==";
    pr.hash = "aGFzaA==";
    b.wan.passwordRecord = pr;
    b.wan.gatedPaths = {*GateRule::parse(JVal("GET /api/download")), *GateRule::parse(JVal("/api/files/*"))};
    b.wan.trustedProxies = {"127.0.0.1"};
    b.wan.requireTls = true;
    b.agentToken = "agent-token-xyz";
    b.tls.pfx = "C:\\certs\\a.pfx";
    b.tls.passwordEnv = "ANFS_PFX_PW";
    b.corsAllow = {"https://a.example", "https://b.example"};
    b.rev = 42;
    b.raw = JVal::object({{"allowRemoteBridge", true}, {"denyPaths", JVal::array({"C:\\secret"})}});
    const Config back = Config::fromJson(b.toJsonText());
    QCOMPARE(diffConfig(b, back), QString());
    QVERIFY(back.rawBool("allowRemoteBridge", false));
    QCOMPARE(back.rawGet("denyPaths").size(), size_t(1));
    // The module keys came first in raw, so they stay first in the file.
    const std::string bText = json::stringify(back.toJval());
    QVERIFY(bText.rfind("{\"allowRemoteBridge\":true,\"denyPaths\":[", 0) == 0);
}

void TestAnfsCoreConfig::configDefaultsAndAbsentKeys()
{
    const Config d = Config::fromJson("{}");
    QCOMPARE(QString::fromStdString(d.mode), QString("off"));
    QVERIFY(d.bindAll && d.exposeFullDrives && d.allowWrite && !d.test);
    QCOMPARE(QString::fromStdString(d.bindAddress), QString("127.0.0.1"));
    QCOMPARE(d.wan.trustedProxies.size(), size_t(2));
    QVERIFY(!d.wan.passwordRecord);
    QVERIFY(d.wan.gatedPaths.empty());
    QCOMPARE(d.rev, int64_t(0));
    SEQ(json::stringify(d.toJval()), kDefaultsWritten);
    SEQ(d.toJsonText(), kDefaultsFileText);
    SEQ(json::stringify(Config().toJval()), kDefaultsWritten);   // a default-constructed Config writes the same

    // Effective ports (C# Config.EffectivePort / EffectiveTlsPort).
    QCOMPARE(Config::fromJson("{\"mode\":\"server\"}").effectivePort(), 8080);
    QCOMPARE(Config::fromJson("{\"mode\":\"server\",\"test\":true}").effectivePort(), 18080);
    QCOMPARE(Config::fromJson("{\"mode\":\"peer\"}").effectivePort(), 8899);
    QCOMPARE(Config::fromJson("{\"mode\":\"peer\",\"test\":true}").effectivePort(), 18899);
    QCOMPARE(Config::fromJson("{\"test\":true}").effectivePort(), 18899);   // off behaves like peer
    QCOMPARE(Config::fromJson("{\"mode\":\"peer\",\"port\":9000}").effectivePort(), 9000);
    QCOMPARE(Config::fromJson("{}").effectiveTlsPort(), 443);
    QCOMPARE(Config::fromJson("{\"test\":true}").effectiveTlsPort(), 18443);

    // C# leniency: numbers and booleans as strings, a URL as a plain string.
    const Config len = Config::fromJson("{\"port\":\"9001\",\"bindAll\":\"FALSE\",\"test\":\"true\",\"nodeId\":17,"
                                        "\"bridgeUrl\":\" http://x \",\"tlsPort\":443.9}");
    QCOMPARE(len.port, 9001);
    QVERIFY(!len.bindAll && len.test);
    QCOMPARE(QString::fromStdString(len.nodeId), QString("17"));
    QCOMPARE(QString::fromStdString(len.bridgeUrl()), QString("http://x"));
    QCOMPARE(len.tlsPort, 443);
    QVERIFY(Config::fromJson("{\"allowWrite\":true,\"exposeFullDrives\":false}").allowWrite);
    QVERIFY(Config::fromJson("{\"allowWrite\":false}").exposeFullDrives);
    const Config off = Config::fromJson("{\"exposeFullDrives\":false,\"allowWrite\":false}");
    const Config off2 = Config::fromJson(off.toJsonText());
    QVERIFY(!off2.exposeFullDrives && !off2.allowWrite);   // explicit false survives a write/read

    // Absent-when-default: these keys disappear from the file when their value is the default.
    const Config a = Config::fromJson("{\"port\":0,\"tlsPort\":\"0\",\"statusPort\":0,\"test\":false,\"agentToken\":\"\","
                                      "\"bridgeUrl\":[],\"bridgeFallbacks\":[\"http://only\"],"
                                      "\"tls\":{\"pfx\":\"\",\"passwordEnv\":\"\",\"other\":1},\"rev\":0,\"keep\":true}");
    SEQ(json::stringify(a.toJval()),
        "{\"bridgeUrl\":\"http://only\",\"keep\":true,\"mode\":\"off\",\"nodeId\":\"\",\"name\":\"\",\"bindAll\":true,"
        "\"exposeFullDrives\":true,\"allowWrite\":true,\"corsAllow\":[],\"shares\":[],\"folders\":{},"
        "\"wan\":{\"gatedPaths\":[],\"trustedProxies\":[\"127.0.0.1\",\"::1\"],\"requireTls\":false}}");

    // bindAddress is written only when bindAll is false; with bindAll true a raw value just stays as it was.
    Config ba = Config::fromJson("{\"bindAddress\":\"10.1.1.1\"}");
    QVERIFY(json::stringify(ba.toJval()).find("\"bindAddress\":\"10.1.1.1\"") != std::string::npos);
    ba.bindAll = true;
    ba.bindAddress = "10.2.2.2";
    QVERIFY(json::stringify(ba.toJval()).find("\"bindAddress\":\"10.1.1.1\"") != std::string::npos);
    ba.bindAll = false;
    QVERIFY(json::stringify(ba.toJval()).find("\"bindAddress\":\"10.2.2.2\"") != std::string::npos);

    // Folder keys are case-insensitive and keep their first spelling and position (C# OrdinalIgnoreCase).
    Config f = Config::fromJson("{\"folders\":{\"DropBox\":\"A\",\"x\":\"1\",\"dropbox\":\"B\"}}");
    QCOMPARE(f.folders.size(), size_t(2));
    QCOMPARE(QString::fromStdString(f.dropbox()), QString("B"));
    f.setFolder("DROPBOX", "C");
    QCOMPARE(QString::fromStdString(f.folders[0].first), QString("DropBox"));
    SEQ(json::stringify(f.toJval().get("folders")), "{\"DropBox\":\"C\",\"x\":\"1\",\"dropbox\":\"B\"}");

    // A duplicate fallback is dropped; extra URLs become bridgeFallbacks.
    Config br = Config::fromJson("{\"bridgeUrl\":\"http://a\",\"bridgeFallbacks\":[\"http://a\",\"http://b\"]}");
    QCOMPARE(br.bridgeUrls.size(), size_t(2));
    SEQ(json::stringify(br.toJval().get("bridgeFallbacks")), "[\"http://b\"]");
    br.bridgeUrls.clear();
    QVERIFY(!br.toJval().has("bridgeUrl") && !br.toJval().has("bridgeFallbacks"));

    // trustedProxies present but null/empty replaces the defaults with nothing (C# ContainsKey).
    QVERIFY(Config::fromJson("{\"wan\":{\"trustedProxies\":null}}").wan.trustedProxies.empty());
    QCOMPARE(Config::fromJson("{\"wan\":{}}").wan.trustedProxies.size(), size_t(2));
}

void TestAnfsCoreConfig::configValidation()
{
    auto err = [](const char* text) -> QString {
        try {
            Config::fromJson(text);
        } catch (const ConfigError& e) {
            return QString::fromUtf8(e.what());
        }
        return QString("no error");
    };
    QCOMPARE(err("{\"mode\":\"bogus\"}"), QString("mode must be off, peer or server (got 'bogus')"));
    QCOMPARE(err("{\"port\":70000}"), QString("port out of range"));
    QCOMPARE(err("{\"port\":-1}"), QString("port out of range"));
    QCOMPARE(err("{\"port\":4294975296}"), QString("port out of range"));   // C# wrapped this to 8000 with an int cast
    QCOMPARE(err("{\"tlsPort\":65536}"), QString("tlsPort out of range"));
    QCOMPARE(err("{\"statusPort\":99999}"), QString("statusPort out of range"));
    QCOMPARE(err("{\"mode\":\"x\",\"port\":70000}"), QString("mode must be off, peer or server (got 'x')"));   // C# order
    QCOMPARE(err("[1]"), QString("invalid JSON: expected a JSON object"));
    QCOMPARE(err("\"peer\""), QString("invalid JSON: expected a JSON object"));
    QVERIFY(err("{ this is not json").startsWith("invalid JSON: "));
    QVERIFY(err("").startsWith("invalid JSON: "));
    QCOMPARE(err("\xEF\xBB\xBF{\"mode\":\"peer\"}"), QString("no error"));   // UTF-8 BOM (Notepad)
    QCOMPARE(QString::fromStdString(Config::fromJson("{\"mode\":\"\"}").mode), QString("off"));
    QCOMPARE(QString::fromStdString(Config::fromJson("{\"mode\":\"\\tPEER \"}").mode), QString("peer"));

    // validate() normalises in place; toJval() refuses an invalid config without changing it.
    Config c;
    c.mode = " Server";
    c.validate();
    QCOMPARE(QString::fromStdString(c.mode), QString("server"));
    c.mode = "nonsense";
    bool threw = false;
    try {
        c.toJval();
    } catch (const ConfigError&) {
        threw = true;
    }
    QVERIFY(threw);
    QCOMPARE(QString::fromStdString(c.mode), QString("nonsense"));
    c.mode = "PEER";
    QCOMPARE(QString::fromStdString(c.toJval().getStr("mode")), QString("peer"));
    c.port = 65536;
    threw = false;
    try {
        c.toJsonText();
    } catch (const ConfigError& e) {
        threw = QString::fromUtf8(e.what()) == "port out of range";
    }
    QVERIFY(threw);
    QVERIFY(Config::fromJval(JVal::object({{"mode", "server"}})).mode == "server");
    threw = false;
    try {
        Config::fromJval(JVal::array());
    } catch (const ConfigError&) {
        threw = true;
    }
    QVERIFY(threw);
}

void TestAnfsCoreConfig::configSafeAndRaw()
{
    const Config c = Config::fromJson(kFullConfig);
    const JVal safe = c.toSafeJval();
    const std::string s = json::stringify(safe);
    QVERIFY(s.find("tok-123") == std::string::npos && s.find("aGFzaA") == std::string::npos && s.find("PFXPW") == std::string::npos);
    QCOMPARE(QString::fromStdString(safe.getStr("agentToken")), QString("[set]"));
    QCOMPARE(QString::fromStdString(safe.get("wan").getStr("passwordRecord")), QString("[set]"));
    QVERIFY(safe.get("tls").has("pfx") && !safe.get("tls").has("passwordEnv"));
    QCOMPARE(QString::fromStdString(c.agentToken), QString("tok-123"));   // the config itself is untouched
    const JVal plain = Config::fromJson("{}").toSafeJval();
    QVERIFY(!plain.has("agentToken") && !plain.get("wan").has("passwordRecord") && !plain.has("tls"));

    // Module keys are read from raw with the C# leniency.
    const Config r = Config::fromJson("{\"allowRemoteBridge\":\"true\",\"webroot\":\"C:\\\\webroot\",\"watchdogIntervalSec\":\"30\","
                                      "\"denyPaths\":[\"C:\\\\a\"]}");
    QVERIFY(r.rawBool("allowRemoteBridge", false));
    QVERIFY(!r.rawBool("missing", false) && r.rawBool("missing", true));
    QCOMPARE(QString::fromStdString(r.rawStr("webroot")), QString("C:\\webroot"));
    QCOMPARE(QString::fromStdString(r.rawStr("missing", "dflt")), QString("dflt"));
    QCOMPARE(r.rawInt("watchdogIntervalSec", 60), int64_t(30));
    QCOMPARE(r.rawInt("missing", 60), int64_t(60));
    QVERIFY(r.rawGet("denyPaths").isArray());
    QVERIFY(r.rawGet("missing").isNull());

    const std::string dd = defaultDataDir();
    QVERIFY2(QString::fromStdString(dd).endsWith("\\StarRoot\\anfs") && dd.size() > 20, dd.c_str());
    QVERIFY(dd.find("\\\\") == std::string::npos);
}

void TestAnfsCoreConfig::gateRulesAndPasswordRecord()
{
    auto rule = [](const JVal& v) { return GateRule::parse(v); };
    QCOMPARE(QString::fromStdString(rule(JVal("POST /api/upload/{id}"))->toString()), QString("POST /api/upload/{id}"));
    QCOMPARE(QString::fromStdString(rule(JVal("post   /api/x "))->toString()), QString("POST /api/x"));
    QCOMPARE(QString::fromStdString(rule(JVal("/api/x"))->toString()), QString("* /api/x"));
    QCOMPARE(QString::fromStdString(rule(JVal("* /api/z"))->toString()), QString("* /api/z"));
    QCOMPARE(QString::fromStdString(rule(JVal("api/q"))->toString()), QString("* /api/q"));
    QCOMPARE(QString::fromStdString(rule(JVal("/api/a b"))->toString()), QString("* /api/a b"));   // starts with '/': all pattern
    QCOMPARE(QString::fromStdString(rule(JVal::object({{"method", ""}, {"pattern", "/p"}}))->toString()), QString("* /p"));
    QCOMPARE(QString::fromStdString(rule(JVal::object({{"pattern", "/p"}}))->toString()), QString("* /p"));
    QVERIFY(!rule(JVal("")) && !rule(JVal("   ")) && !rule(JVal(5)) && !rule(JVal()) && !rule(JVal::object({{"method", "GET"}})));

    const GateRule up = *rule(JVal("POST,put /api/up/{id}"));
    QVERIFY(up.matches("POST", "/api/up/42"));
    QVERIFY(up.matches("put", "/api/up/42/"));            // methods case-insensitive; empty segments dropped
    QVERIFY(!up.matches("GET", "/api/up/42"));
    QVERIFY(!up.matches("HEAD", "/api/up/42"));           // HEAD is the gate's business, not the rule's
    QVERIFY(!up.matches("POST", "/api/up"));
    QVERIFY(!up.matches("POST", "/api/up/42/x"));
    const GateRule files = *rule(JVal("/API/Files/*"));
    QVERIFY(files.matches("GET", "/api/files"));          // a rest segment also matches nothing (C# PathPattern)
    QVERIFY(files.matches("DELETE", "/api/files/a/b/c"));
    QVERIFY(files.matches("GET", "/api/%66iles/x"));      // literals compare with the percent-decoded segment
    QVERIFY(!files.matches("GET", "/api/filesx/a"));
    QVERIFY(rule(JVal("GET /a/{*rest}"))->matches("GET", "/a/b/c"));
    QVERIFY(!rule(JVal("GET /a/{*rest}/b"))->matches("GET", "/a/b/c"));   // rest only as the last segment
    QVERIFY(rule(JVal("/"))->matches("GET", "/"));
    QVERIFY(!rule(JVal("/"))->matches("GET", "/x"));
    QVERIFY(rule(JVal("/api/{}"))->matches("GET", "/api/{}"));             // "{}" is a literal (C# needs length > 2)
    QVERIFY(!rule(JVal("/api/{}"))->matches("GET", "/api/x"));

    PasswordRecord r;
    QVERIFY(!r.isSet());
    r.algo = "PBKDF2_SHA256";
    r.iterations = 10;
    r.salt = "s";
    r.hash = "h";
    QVERIFY(r.isSet());
    r.algo = "pbkdf2";
    QVERIFY(r.isSet());
    r.algo = "scrypt";
    QVERIFY(!r.isSet());
    r.algo = "pbkdf2-sha256";
    r.salt.clear();
    QVERIFY(!r.isSet());
    const PasswordRecord small{"a", 5, "s", "h"};
    SEQ(json::stringify(small.toJval()), "{\"algo\":\"a\",\"iterations\":5,\"salt\":\"s\",\"hash\":\"h\"}");
    QVERIFY(!PasswordRecord::fromJval(JVal("x")));
    QVERIFY(!PasswordRecord::fromJval(JVal()));
    const auto big = PasswordRecord::fromJval(JVal::object({{"iterations", int64_t(1) << 40}, {"salt", 5}}));
    QVERIFY(big && big->iterations == 2147483647 && big->salt == "5" && big->algo.empty());
    QCOMPARE(PasswordRecord::fromJval(JVal::object({{"iterations", -3}}))->iterations, int64_t(0));
}

void TestAnfsCoreConfig::writeFileAtomicBasics()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath("a/b/c");
    const std::string path = QDir::toNativeSeparators(dir + "/x.json").toStdString();
    std::string err = "stale";
    QVERIFY(writeFileAtomic(path, "one", &err));     // creates the missing folders
    QVERIFY(err.empty());
    QCOMPARE(QByteArray::fromStdString(readAll(dir + "/x.json")), QByteArray("one"));
    QVERIFY(writeFileAtomic(path, std::string(200000, 'z')));
    QCOMPARE(readAll(dir + "/x.json").size(), size_t(200000));
    QVERIFY(writeFileAtomic(path, ""));
    QCOMPARE(QFileInfo(dir + "/x.json").size(), qint64(0));
    QVERIFY(tmpFiles(dir).isEmpty());

    // A folder in the way: the replace fails, the error names it, the temp file is gone.
    QVERIFY(QDir(dir).mkpath("isdir"));
    QVERIFY(!writeFileAtomic(QDir::toNativeSeparators(dir + "/isdir").toStdString(), "x", &err));
    QVERIFY2(err.find("cannot replace") != std::string::npos, err.c_str());
    QVERIFY(tmpFiles(dir).isEmpty());
    QVERIFY(!writeFileAtomic("", "x", &err));
    QVERIFY(!writeFileAtomic("Z:\\no\\such\\drive\\?*\\x.json", "x", &err));
    QVERIFY(!err.empty());

    // A reader holding the file without FILE_SHARE_DELETE only delays the replace (retried), it does not fail it.
    const std::wstring w = wpath(dir + "/x.json");
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    std::thread release([h] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CloseHandle(h);
    });
    QElapsedTimer t;
    t.start();
    const bool ok = writeFileAtomic(path, "after", &err);
    release.join();
    QVERIFY2(ok, err.c_str());
    QVERIFY(t.elapsed() >= 100);
    QCOMPARE(QByteArray::fromStdString(readAll(dir + "/x.json")), QByteArray("after"));
}

void TestAnfsCoreConfig::storeLoadSaveUpdate()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath("anfs.json");
    ConfigStore s(QDir::toNativeSeparators(path).toStdString());
    QVERIFY(s.current() && s.current()->mode == "off");
    QCOMPARE(s.version(), int64_t(0));
    QVERIFY(!s.hasChangedOnDisk());                 // no file and never had one: not a change
    QVERIFY(s.load());                              // missing file: defaults
    QCOMPARE(s.version(), int64_t(1));
    QVERIFY(!QFile::exists(path));

    Config c;
    c.mode = "peer";
    c.name = "one";
    c.test = true;
    s.save(c);
    QVERIFY(QFile::exists(path));
    SEQ(readAll(path), c.toJsonText());
    QCOMPARE(s.version(), int64_t(2));
    QVERIFY(!s.hasChangedOnDisk());                 // own save is not an external change
    QVERIFY(s.lastError().empty());

    const auto u = s.update([](Config& x) {
        x.name = "two";
        x.shares.push_back(Share{"a", "A", "D:\\a"});
    });
    QCOMPARE(QString::fromStdString(u->name), QString("two"));
    QCOMPARE(u.get(), s.current().get());
    ConfigStore s2(QDir::toNativeSeparators(path).toStdString());
    QVERIFY(s2.load());
    QCOMPARE(QString::fromStdString(s2.current()->name), QString("two"));
    QCOMPARE(QString::fromStdString(s2.current()->mode), QString("peer"));
    QCOMPARE(s2.current()->shares.size(), size_t(1));

    // bumpRev increments rev on the config being written.
    QCOMPARE(s.update([](Config&) {}, true)->rev, int64_t(1));
    QCOMPARE(s.update([](Config&) {}, true)->rev, int64_t(2));
    QCOMPARE(s.update([](Config&) {})->rev, int64_t(2));

    // An invalid update is refused and the file is untouched.
    const std::string before = readAll(path);
    bool threw = false;
    try {
        s.update([](Config& x) { x.mode = "nonsense"; });
    } catch (const ConfigError&) {
        threw = true;
    }
    QVERIFY(threw);
    SEQ(readAll(path), before);
    QVERIFY(s2.load() && s2.current()->mode == "peer");

    // Bad JSON keeps the old snapshot (same object) and records the error; a fix clears it.
    QVERIFY(writeFileAtomic(path.toStdString(), "{ this is not json"));
    const auto old = s.current();
    const int64_t v = s.version();
    QVERIFY(s.hasChangedOnDisk());
    QVERIFY(!s.load());
    QCOMPARE(s.current().get(), old.get());
    QCOMPARE(s.version(), v);
    QVERIFY(QString::fromStdString(s.lastError()).startsWith("invalid JSON: "));
    QVERIFY(!s.hasChangedOnDisk());                 // the bad file's signature is recorded: no reload storm
    QVERIFY(writeFileAtomic(path.toStdString(), "{\"mode\":\"server\",\"test\":true}"));
    QVERIFY(s.load() && s.current()->mode == "server" && s.lastError().empty());
    QVERIFY(writeFileAtomic(path.toStdString(), "{\"mode\":\"weird\"}"));
    QVERIFY(!s.load() && s.current()->mode == "server");
    QCOMPARE(QString::fromStdString(s.lastError()), QString("mode must be off, peer or server (got 'weird')"));

    // A file that vanishes after having existed keeps the previous config, and every poll retries.
    QVERIFY(writeFileAtomic(path.toStdString(), "{\"mode\":\"peer\"}"));
    QVERIFY(s.load());
    QVERIFY(QFile::remove(path));
    QVERIFY(s.hasChangedOnDisk());
    QVERIFY(!s.load());
    QCOMPARE(QString::fromStdString(s.lastError()), QString("config file is missing, keeping previous settings"));
    QCOMPARE(QString::fromStdString(s.current()->mode), QString("peer"));
    QVERIFY(s.hasChangedOnDisk());
    QVERIFY(writeFileAtomic(path.toStdString(), "{\"mode\":\"server\"}"));
    QVERIFY(s.load() && s.current()->mode == "server");

    // UTF-16 files (StreamReader detects the BOM in C#) and a UTF-8 BOM.
    const std::u16string u16 = u"{\"mode\":\"peer\",\"name\":\"\u00e9t\u00e9\"}";
    std::string le = "\xFF\xFE", be = "\xFE\xFF";
    for (char16_t ch : u16) {
        le += static_cast<char>(ch & 0xFF);
        le += static_cast<char>(ch >> 8);
        be += static_cast<char>(ch >> 8);
        be += static_cast<char>(ch & 0xFF);
    }
    QVERIFY(writeFileAtomic(path.toStdString(), le));
    QVERIFY(s.load());
    SEQ(s.current()->name, "\xC3\xA9t\xC3\xA9");
    QVERIFY(writeFileAtomic(path.toStdString(), be));
    QVERIFY(s.load() && s.current()->mode == "peer");
    QVERIFY(writeFileAtomic(path.toStdString(), "\xEF\xBB\xBF{\"mode\":\"server\"}"));
    QVERIFY(s.load() && s.current()->mode == "server");

    // A save that cannot be written throws, records the error and keeps the snapshot.
    QVERIFY(QFile::remove(path));
    QVERIFY(QDir(tmp.path()).mkpath("anfs.json"));   // a folder where the file should be
    const auto keep = s.current();
    threw = false;
    try {
        s.save(Config());
    } catch (const ConfigError& e) {
        threw = QString::fromUtf8(e.what()).startsWith("cannot write config: ");
    }
    QVERIFY(threw);
    QCOMPARE(s.current().get(), keep.get());
    QVERIFY(QString::fromStdString(s.lastError()).startsWith("cannot write config: "));
    QVERIFY(tmpFiles(tmp.path()).isEmpty());
}

void TestAnfsCoreConfig::storeListeners()
{
    QTemporaryDir tmp;
    const std::string path = QDir::toNativeSeparators(tmp.filePath("anfs.json")).toStdString();
    ConfigStore s(path);
    std::vector<std::string> seen;
    std::mutex mu;
    const int id = s.subscribe([&](std::shared_ptr<const Config> c) {
        std::lock_guard<std::mutex> g(mu);
        seen.push_back(c->name);
    });
    // A subscriber may write through the store (locks are released before it runs); the guard ends the recursion.
    std::atomic<int> nested{0};
    const int id2 = s.subscribe([&](std::shared_ptr<const Config> c) {
        if (c->name == "trigger" && nested.fetch_add(1) == 0) s.update([](Config& x) { x.name = "from-listener"; });
    });
    Config c;
    c.name = "first";
    s.save(c);
    s.update([](Config& x) { x.name = "trigger"; });
    QCOMPARE(QString::fromStdString(s.current()->name), QString("from-listener"));
    QVERIFY(writeFileAtomic(path, "{\"name\":\"external\"}"));
    QVERIFY(s.load());
    {
        std::lock_guard<std::mutex> g(mu);
        // first, then the nested update's notification (newest snapshot), then the outer one (also newest), external...
        QVERIFY(seen.size() >= 4);
        QCOMPARE(QString::fromStdString(seen.front()), QString("first"));
        QVERIFY(std::find(seen.begin(), seen.end(), "from-listener") != seen.end());
        QVERIFY(std::find(seen.begin(), seen.end(), "external") != seen.end());
    }
    s.unsubscribe(id);
    s.unsubscribe(id2);
    const size_t n = seen.size();
    s.update([](Config& x) { x.name = "quiet"; });
    QCOMPARE(seen.size(), n);
    // A throwing subscriber does not undo the save.
    s.subscribe([](std::shared_ptr<const Config>) { throw std::runtime_error("boom"); });
    s.update([](Config& x) { x.name = "after-throw"; });
    QCOMPARE(QString::fromStdString(s.current()->name), QString("after-throw"));
}

void TestAnfsCoreConfig::storeSeesSameSizeSameMtimeRewrite()
{
    QTemporaryDir tmp;
    const QString qpath = tmp.filePath("anfs.json");
    const std::string path = QDir::toNativeSeparators(qpath).toStdString();
    ConfigStore s(path);
    Config a;
    a.name = "aaaa";
    s.save(a);
    const std::wstring w = wpath(qpath);
    FILETIME ft{};
    {
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        QVERIFY(GetFileTime(h, nullptr, nullptr, &ft));
        CloseHandle(h);
    }
    Config b = a;
    b.name = "bbbb";
    const std::string text = b.toJsonText();
    QCOMPARE(text.size(), a.toJsonText().size());
    QVERIFY(writeFileAtomic(path, text));
    {
        HANDLE h = CreateFileW(w.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        QVERIFY(SetFileTime(h, nullptr, nullptr, &ft));   // same size, same mtime: only the hash can tell
        CloseHandle(h);
    }
    QVERIFY(s.hasChangedOnDisk());
    QVERIFY(s.load());
    QCOMPARE(QString::fromStdString(s.current()->name), QString("bbbb"));
    QVERIFY(!s.hasChangedOnDisk());
}

void TestAnfsCoreConfig::storeAtomicUnderConcurrentReaders()
{
    QTemporaryDir tmp;
    const QString qpath = tmp.filePath("anfs.json");
    const std::string path = QDir::toNativeSeparators(qpath).toStdString();
    const std::wstring w = wpath(qpath);
    ConfigStore s(path);
    s.save(Config());

    std::atomic<bool> done{false};
    std::atomic<int> reads{0}, naiveReads{0}, openFails{0}, torn{0}, inconsistent{0}, snapReads{0};
    // A complete file always has name == "n-<counter>" (both written by one update).
    auto consistent = [](const Config& c) {
        const int64_t n = c.rawInt("counter", 0);
        return n == 0 ? c.name.empty() : c.name == "n-" + std::to_string(n);
    };
    auto fileReader = [&](DWORD share, std::atomic<int>& counter, int pauseMs) {
        while (!done.load()) {
            HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                ++openFails;   // the replace window, or blocked by the writer's own open: retried
                continue;
            }
            std::string text(65536, '\0');
            DWORD n = 0;
            const BOOL ok = ReadFile(h, &text[0], static_cast<DWORD>(text.size()), &n, nullptr);
            CloseHandle(h);
            if (!ok) continue;
            text.resize(n);
            ++counter;
            try {
                if (!consistent(Config::fromJson(text))) ++inconsistent;
            } catch (const ConfigError&) {
                ++torn;
            }
            if (pauseMs) std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
        }
    };
    std::vector<std::thread> readers;
    readers.emplace_back(fileReader, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, std::ref(reads), 0);
    readers.emplace_back(fileReader, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, std::ref(reads), 0);
    readers.emplace_back(fileReader, DWORD(FILE_SHARE_READ), std::ref(naiveReads), 2);   // File.ReadAllText-style
    readers.emplace_back([&] {
        while (!done.load()) {
            const auto c = s.current();
            ++snapReads;
            if (!consistent(*c)) ++inconsistent;
        }
    });

    // 4 updaters x 40 increments: conditional writes mean no increment is lost.
    std::atomic<int> updateErrors{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&] {
            for (int i = 0; i < 40; ++i) {
                try {
                    s.update([](Config& c) {
                        const int64_t n = c.rawInt("counter", 0) + 1;
                        c.raw.set("counter", n);
                        c.name = "n-" + std::to_string(n);
                    });
                } catch (const std::exception&) {
                    ++updateErrors;
                }
            }
        });
    }
    for (auto& t : writers) t.join();
    done = true;
    for (auto& t : readers) t.join();

    qInfo("atomic: %d shared reads, %d naive reads, %d snapshot reads, %d open retries", reads.load(), naiveReads.load(),
          snapReads.load(), openFails.load());
    QCOMPARE(updateErrors.load(), 0);
    QCOMPARE(s.current()->rawInt("counter", 0), int64_t(160));
    QCOMPARE(Config::fromJson(readAll(qpath)).rawInt("counter", 0), int64_t(160));
    QVERIFY(reads.load() > 0 && naiveReads.load() > 0 && snapReads.load() > 0);
    QCOMPARE(torn.load(), 0);
    QCOMPARE(inconsistent.load(), 0);
    QVERIFY(tmpFiles(tmp.path()).isEmpty());
}

void TestAnfsCoreConfig::watcherSeesChangeWithinTwoSecondsOnInjectedClock()
{
    QTemporaryDir tmp;
    const std::string path = QDir::toNativeSeparators(tmp.filePath("anfs.json")).toStdString();
    ManualClock mc;
    ScopedClock scope(mc);   // declared before the store and watcher, so it outlives the watcher thread
    ConfigStore store(path);
    store.save(Config());
    std::atomic<int> changes{0};
    std::atomic<int64_t> seenAt{-1};
    store.subscribe([&](std::shared_ptr<const Config>) {
        seenAt = mc.monoMs();
        ++changes;
    });
    std::mutex errMu;
    std::vector<std::string> errors;
    {
        ConfigWatcher w(store);   // default 2000 ms
        w.onError([&](const std::string& e) {
            std::lock_guard<std::mutex> g(errMu);
            errors.push_back(e);
        });
        w.start();
        QThread::msleep(100);     // the thread is now waiting for 2000 ms of manual time
        const int64_t t0 = mc.monoMs();
        QVERIFY(writeFileAtomic(path, "{\"mode\":\"peer\",\"test\":true}\r\n"));   // another writer (anfs config set)

        QThread::msleep(300);
        QCOMPARE(changes.load(), 0);   // real time passing does nothing: the watcher runs on the injected clock
        while (mc.monoMs() - t0 < 1980) {
            mc.advance(20);
            QThread::msleep(1);
        }
        QThread::msleep(150);
        QCOMPARE(changes.load(), 0);   // not at 1.98 s
        mc.advance(20);
        QVERIFY(waitReal([&] { return changes.load() == 1; }, 3000));   // at 2.00 s
        QCOMPARE(seenAt.load() - t0, int64_t(2000));
        QCOMPARE(QString::fromStdString(store.current()->mode), QString("peer"));

        // Idle intervals cause no reload.
        for (int i = 0; i < 3; ++i) {
            mc.advance(2000);
            QThread::msleep(60);
        }
        QCOMPARE(changes.load(), 1);

        // A bad file is reported once, however many intervals pass, and keeps the old config.
        QVERIFY(writeFileAtomic(path, "{\"mode\": oops"));
        mc.advance(2000);
        QVERIFY(waitReal([&] { std::lock_guard<std::mutex> g(errMu); return errors.size() == 1; }, 3000));
        for (int i = 0; i < 3; ++i) {
            mc.advance(2000);
            QThread::msleep(60);
        }
        {
            std::lock_guard<std::mutex> g(errMu);
            QCOMPARE(errors.size(), size_t(1));
            QVERIFY(QString::fromStdString(errors[0]).startsWith("invalid JSON: "));
        }
        QCOMPARE(changes.load(), 1);
        QCOMPARE(QString::fromStdString(store.current()->mode), QString("peer"));

        // The fix is picked up at the next interval; a second distinct error would be reported again.
        QVERIFY(writeFileAtomic(path, "{\"mode\":\"server\",\"test\":true,\"name\":\"x\"}"));
        mc.advance(2000);
        QVERIFY(waitReal([&] { return changes.load() == 2; }, 3000));
        QCOMPARE(QString::fromStdString(store.current()->mode), QString("server"));

        // Our own save notifies once (from the store) and is not seen again by the poller.
        store.update([](Config& c) { c.name = "by-us"; });
        QCOMPARE(changes.load(), 3);
        for (int i = 0; i < 3; ++i) {
            mc.advance(2000);
            QThread::msleep(60);
        }
        QCOMPARE(changes.load(), 3);

        QElapsedTimer t;
        t.start();
        w.stop();
        QVERIFY2(t.elapsed() < 1000, "stop() returns promptly while the thread waits on the manual clock");
        w.start();                // restartable
        QThread::msleep(50);
    }   // destructor stops it again
}

void TestAnfsCoreConfig::watcherStopsPromptlyOnSystemClock()
{
    QTemporaryDir tmp;
    const std::string path = QDir::toNativeSeparators(tmp.filePath("anfs.json")).toStdString();
    ConfigStore store(path);
    store.save(Config());
    std::atomic<int> changes{0};
    store.subscribe([&](std::shared_ptr<const Config>) { ++changes; });
    ConfigWatcher w(store, 2000);
    w.start();
    QThread::msleep(50);
    QElapsedTimer t;
    t.start();
    w.stop();
    QVERIFY(t.elapsed() < 500);
    // poll() is one synchronous check: no change, then an external change.
    w.poll();
    QCOMPARE(changes.load(), 0);
    QVERIFY(writeFileAtomic(path, "{\"mode\":\"peer\"}"));
    w.poll();
    QCOMPARE(changes.load(), 1);
    w.poll();
    QCOMPARE(changes.load(), 1);
    // A short real interval works too (C# Config_Watcher used 40 ms).
    ConfigWatcher fast(store, 40);
    fast.start();
    QVERIFY(writeFileAtomic(path, "{\"mode\":\"server\"}"));
    QVERIFY(waitReal([&] { return changes.load() == 2; }, 3000));
}

// ---- log ----------------------------------------------------------------------------------------------------------------

void TestAnfsCoreConfig::logRedactVectors()
{
    for (const RedactCase& c : kRedact) SEQ(Log::redact(c.in), c.out);
    // Linear on a long line (std::regex on MSVC would recurse per character here).
    std::string longLine(2 << 20, 'a');
    longLine += " token=abc";
    QElapsedTimer t;
    t.start();
    const std::string r = Log::redact(longLine);
    QVERIFY(r.size() > longLine.size() - 10 && r.find("abc") == std::string::npos);
    QVERIFY(t.elapsed() < 2000);
}

void TestAnfsCoreConfig::logFileFormatSecretsAndCounters()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath("logs");
    ManualClock mc(localEpochMs(2026, 1, 14, 9, 5, 7, 42));
    ScopedClock scope(mc);
    Log log(QDir::toNativeSeparators(dir).toStdString());
    QCOMPARE(QString::fromStdString(log.dir()), QDir::toNativeSeparators(dir));
    QCOMPARE(QString::fromStdString(log.currentFile()), QDir::toNativeSeparators(dir + "/anfs-20260114.log"));
    QCOMPARE(log.errorCount(), 0);
    QVERIFY(log.lastError().empty());

    const std::string token = "tok-SECRET-123456";
    log.registerSecret(token);
    log.registerSecret("short");   // < 6 characters: ignored (would mask ordinary words)
    log.info("listening on 18899");
    log.debug("not written at the default level");
    log.warn("multi\r\nline\nmessage");
    log.info("peer said " + token + " twice " + token);
    log.info("short words stay");
    mc.advance(1500);
    log.error("relay failed: x-agent-auth: " + token);
    log.setMinLevel(LogLevel::Error);
    log.info("filtered");
    log.state("mode applied: peer");   // state changes are always written
    log.setMinLevel(LogLevel::Debug);
    log.debug("now debug");
    log.flush();

    QCOMPARE(log.errorCount(), 1);
    SEQ(log.lastError(), "relay failed: x-agent-auth: [redacted]");
    QCOMPARE(log.lastErrorAtMs(), localEpochMs(2026, 1, 14, 9, 5, 8, 542));

    const std::string text = readAll(dir + "/anfs-20260114.log");
    QVERIFY(text.find(token) == std::string::npos);
    QVERIFY(text.find("SECRET") == std::string::npos);
    const std::vector<std::string> ls = lines(text);
    QCOMPARE(ls.size(), size_t(7));   // the two filtered lines are absent; the trailing CRLF adds no line
    SEQ(ls[0], "2026-01-14 09:05:07.042 INFO  listening on 18899");
    SEQ(ls[1], "2026-01-14 09:05:07.042 WARN  multi  | line | message");
    SEQ(ls[2], "2026-01-14 09:05:07.042 INFO  peer said [secret] twice [secret]");
    SEQ(ls[3], "2026-01-14 09:05:07.042 INFO  short words stay");
    SEQ(ls[4], "2026-01-14 09:05:08.542 ERROR relay failed: x-agent-auth: [redacted]");
    SEQ(ls[5], "2026-01-14 09:05:08.542 INFO  mode applied: peer");
    SEQ(ls[6], "2026-01-14 09:05:08.542 DEBUG now debug");
    QVERIFY(text.find("\r\n") != std::string::npos && text.substr(text.size() - 2) == "\r\n");
    const QRegularExpression rx("^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3} (DEBUG|INFO |WARN |ERROR) ");
    for (const std::string& l : ls) QVERIFY(rx.match(QString::fromStdString(l)).hasMatch());

    // A Log without a folder writes nothing but still counts errors; nullLog() discards.
    Log mem;
    mem.error("e1 password=pw");
    QCOMPARE(mem.errorCount(), 1);
    SEQ(mem.lastError(), "e1 password=\"[redacted]\"");
    QVERIFY(mem.currentFile().empty() && mem.dir().empty());
    mem.flush();
    nullLog().info("discarded");
    nullLog().flush();
    QVERIFY(nullLog().currentFile().empty());

    // A very long message is cut, not queued whole.
    log.info(std::string(200000, 'q'));
    log.flush();
    const std::vector<std::string> ls2 = lines(readAll(dir + "/anfs-20260114.log"));
    QVERIFY(ls2.back().size() < 70000 && ls2.back().find("bytes cut)") != std::string::npos);
}

void TestAnfsCoreConfig::logRotatesAtMidnightAndKeepsFourteen()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath("logs");
    QVERIFY(QDir().mkpath(dir));
    // 20 older daily files plus files that are not ours (the C# rolling names, a near-miss name).
    for (int d = 1; d <= 20; ++d) {
        QFile f(dir + QString("/anfs-202512%1.log").arg(d, 2, 10, QChar('0')));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("old\r\n");
    }
    for (const char* other : {"anfs.log", "anfs.1.log", "anfs-2025120.log", "notes.txt"}) {
        QFile f(dir + "/" + other);
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    ManualClock mc(localEpochMs(2026, 1, 14, 23, 59, 59, 500));
    ScopedClock scope(mc);
    {
        Log log(QDir::toNativeSeparators(dir).toStdString(), 14);
        log.info("before midnight");
        log.flush();
        mc.advance(400);
        log.info("still the 14th");
        mc.advance(200);   // 00:00:00.100 on the 15th
        log.info("after midnight");
        QCOMPARE(QString::fromStdString(log.currentFile()), QDir::toNativeSeparators(dir + "/anfs-20260115.log"));
        log.flush();
    }
    const std::vector<std::string> d14 = lines(readAll(dir + "/anfs-20260114.log"));
    const std::vector<std::string> d15 = lines(readAll(dir + "/anfs-20260115.log"));
    QCOMPARE(d14.size(), size_t(2));
    QVERIFY(d14[0].find("23:59:59.500 INFO  before midnight") != std::string::npos);
    QVERIFY(d14[1].find("23:59:59.900 INFO  still the 14th") != std::string::npos);
    QCOMPARE(d15.size(), size_t(1));
    QVERIFY(d15[0].rfind("2026-01-15 00:00:00.100 INFO  after midnight", 0) == 0);

    QStringList daily = QDir(dir).entryList({"anfs-????????.log"}, QDir::Files, QDir::Name);
    QCOMPARE(daily.size(), 14);   // the newest 14: the two new ones and Dec 9..20
    QCOMPARE(daily.first(), QString("anfs-20251209.log"));
    QCOMPARE(daily.last(), QString("anfs-20260115.log"));
    for (const char* other : {"anfs.log", "anfs.1.log", "anfs-2025120.log", "notes.txt"}) QVERIFY(QFile::exists(dir + "/" + other));
}

void TestAnfsCoreConfig::logBurstNeverLosesErrorsAndAccountsForDrops()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath("logs");
    ManualClock mc(localEpochMs(2026, 1, 14, 12, 0, 0, 0));   // one date for the whole burst
    ScopedClock scope(mc);
    constexpr int kThreads = 4, kPerThread = 50000, kErrorEvery = 1000;
    int64_t burstMs = 0;
    {
        Log log(QDir::toNativeSeparators(dir).toStdString());
        QElapsedTimer t;
        t.start();
        std::vector<std::thread> th;
        for (int k = 0; k < kThreads; ++k) {
            th.emplace_back([&log, k] {
                for (int i = 0; i < kPerThread; ++i) {
                    if (i % kErrorEvery == 0) log.error("E t" + std::to_string(k) + " " + std::to_string(i));
                    else log.info("I t" + std::to_string(k) + " " + std::to_string(i) + " some payload text");
                }
            });
        }
        for (auto& x : th) x.join();
        burstMs = t.elapsed();
        log.flush();
    }
    const std::vector<std::string> ls = lines(readAll(dir + "/anfs-20260114.log"));
    int infos = 0, errors = 0;
    int64_t dropped = 0;
    const std::string marker = "log queue full: dropped ";
    for (const std::string& l : ls) {
        if (l.find(" INFO  I t") != std::string::npos) ++infos;
        else if (l.find(" ERROR E t") != std::string::npos) ++errors;
        else if (const size_t p = l.find(marker); p != std::string::npos) dropped += std::stoll(l.substr(p + marker.size()));
    }
    const int totalErrors = kThreads * (kPerThread / kErrorEvery);
    const int totalInfos = kThreads * kPerThread - totalErrors;
    qInfo("burst: %d lines from %d threads in %lld ms; %d info written, %lld dropped", kThreads * kPerThread, kThreads,
          static_cast<long long>(burstMs), infos, static_cast<long long>(dropped));
    QCOMPARE(errors, totalErrors);                     // Warn/Error lines are never the ones dropped
    QCOMPARE(infos + dropped, int64_t(totalInfos));    // every Info line is either written or counted in a marker
    QVERIFY(burstMs < 20000);
}

void TestAnfsCoreConfig::logConsoleRateLimit()
{
    QTemporaryDir tmp;
    const QString out = tmp.filePath("stdout.txt");
    ManualClock mc(localEpochMs(2026, 1, 14, 12, 0, 0, 0));
    ScopedClock scope(mc);
    std::fflush(stdout);
    const int saved = _dup(_fileno(stdout));
    const int fd = _wopen(wpath(out).c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    QVERIFY(saved >= 0 && fd >= 0);
    // Until stdout is restored nothing may print to it (no QVERIFY): results are checked afterwards.
    _dup2(fd, _fileno(stdout));
    {
        Log log;   // console only
        log.setConsole(true);
        for (int i = 0; i < 50; ++i) log.warn("w" + std::to_string(i));
        for (int i = 0; i < 10; ++i) log.info("info is file-only " + std::to_string(i));
        log.state("state line");   // 51st console line: suppressed too
        log.flush();
        mc.advance(10000);         // next window: the suppressed count, then the line
        log.error("after window token=abc");
        log.flush();
        log.setConsole(false);
        log.error("console off");
        log.flush();
    }
    std::fflush(stdout);
    _dup2(saved, _fileno(stdout));
    _close(saved);
    _close(fd);

    const std::vector<std::string> ls = lines(readAll(out));
    QCOMPARE(ls.size(), size_t(22));
    for (int i = 0; i < 20; ++i) QVERIFY(ls[size_t(i)].find("WARN  w" + std::to_string(i)) != std::string::npos);
    QVERIFY2(ls[20].find("WARN  31 console lines suppressed") != std::string::npos, ls[20].c_str());
    QVERIFY(ls[21].find("ERROR after window token=\"[redacted]\"") != std::string::npos);
    QCOMPARE(countContaining(ls, "info is file-only"), 0);
    QCOMPARE(countContaining(ls, "console off"), 0);
}

void TestAnfsCoreConfig::logCallersNeverBlockOnStalledStdout()
{
    QTemporaryDir tmp;
    const QString dir = tmp.filePath("logs");
    ManualClock mc(localEpochMs(2026, 1, 14, 12, 0, 0, 0));
    ScopedClock scope(mc);
    int fds[2] = {-1, -1};
    QVERIFY(_pipe(fds, 4096, _O_BINARY) == 0);
    std::fflush(stdout);
    const int saved = _dup(_fileno(stdout));
    QVERIFY(saved >= 0);
    // stdout becomes a pipe nobody reads. Until it is drained nothing in this thread may touch stdout (the console
    // thread holds the CRT stream lock while it is stuck), so results are kept in locals and checked afterwards.
    _dup2(fds[1], _fileno(stdout));
    int64_t burstMs = -1;
    bool fileComplete = false;
    int fileLines = 0;
    {
        Log log(QDir::toNativeSeparators(dir).toStdString());
        log.setConsole(true);
        const std::string big(4000, 'x');
        for (int w = 0; w < 3; ++w) {   // 60 console lines of 4 KB: far more than the pipe buffers
            for (int i = 0; i < 20; ++i) log.warn("stall " + big);
            mc.advance(10000);
        }
        QThread::msleep(200);           // the console thread is now blocked in fwrite
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 5000; ++i) {
            log.warn("after " + std::to_string(i));
            if (i % 20 == 19) mc.advance(10000);   // keep the rate limit open: console lines pile up and drop
        }
        burstMs = t.elapsed();
        // The file writer does not wait for the console: all 5,060 lines reach the file while stdout is stuck.
        fileComplete = waitReal([&] {
            fileLines = static_cast<int>(lines(readAll(dir + "/anfs-20260114.log")).size());
            return fileLines >= 5060;
        }, 10000);
        std::thread drain([&] {
            char buf[65536];
            while (_read(fds[0], buf, sizeof buf) > 0) {
            }
        });
        log.flush();                    // the console catches up once the pipe is read
        std::fflush(stdout);
        _dup2(saved, _fileno(stdout));
        _close(saved);
        _close(fds[1]);                 // last write end: the drain thread sees EOF
        drain.join();
        _close(fds[0]);
    }
    QVERIFY2(burstMs >= 0 && burstMs < 2000, qPrintable(QString("5000 writes took %1 ms with stdout stalled").arg(burstMs)));
    QVERIFY2(fileComplete, qPrintable(QString("%1 of 5060 lines in the file while stdout was stalled").arg(fileLines)));
}

// ---- svc / ids / clock --------------------------------------------------------------------------------------------------

namespace {
struct IAlpha {
    virtual ~IAlpha() = default;
    virtual int value() const = 0;
};
struct Alpha : IAlpha {
    explicit Alpha(int v) : v(v) {}
    int value() const override { return v; }
    int v;
};
struct IBeta {
    virtual ~IBeta() = default;
};
struct Beta : IBeta {};
// Its destructor uses the registry: replacing or removing it must not deadlock on the registry lock.
struct Reentrant {
    ~Reentrant()
    {
        (void)svcGet<IAlpha>();
        svcRemove<IBeta>();
    }
};
} // namespace

void TestAnfsCoreConfig::svcRegistry()
{
    QVERIFY(!svcGet<IAlpha>() && !svcHas<IAlpha>());
    auto a1 = std::make_shared<Alpha>(1);
    svcSet<IAlpha>(a1);
    QVERIFY(svcHas<IAlpha>());
    QCOMPARE(svcGet<IAlpha>()->value(), 1);
    QVERIFY(!svcGet<IBeta>());   // keyed by type

    // removeIf never removes a successor registered meanwhile (a module stopping after its replacement started).
    auto a2 = std::make_shared<Alpha>(2);
    svcSet<IAlpha>(a2);
    svcRemoveIf<IAlpha>(a1);
    QCOMPARE(svcGet<IAlpha>()->value(), 2);
    svcRemoveIf<IAlpha>(a2);
    QVERIFY(!svcHas<IAlpha>());

    // A consumer holding a service keeps it alive after the module dropped it.
    svcSet<IAlpha>(std::make_shared<Alpha>(3));
    std::shared_ptr<IAlpha> held = svcGet<IAlpha>();
    svcRemove<IAlpha>();
    QCOMPARE(held->value(), 3);
    svcSet<IAlpha>(std::shared_ptr<IAlpha>());   // set(nullptr) removes (C# Svc.Set(null))
    QVERIFY(!svcHas<IAlpha>());

    auto f = std::async(std::launch::async, [] {
        svcSet<IBeta>(std::make_shared<Beta>());
        svcSet<Reentrant>(std::make_shared<Reentrant>());
        svcSet<Reentrant>(std::make_shared<Reentrant>());   // the replaced one dies outside the lock
        svcSet<IBeta>(std::make_shared<Beta>());
        svcRemove<Reentrant>();
        svcSet<Reentrant>(std::make_shared<Reentrant>());
        Svc::clear();
        return true;
    });
    QVERIFY2(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "registry deadlocked in a service destructor");
    QVERIFY(!svcHas<IBeta>() && !svcHas<Reentrant>());

    // Readers and writers in parallel.
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i)
        th.emplace_back([&] {
            while (!stop) {
                auto s = svcGet<IAlpha>();
                if (s && (s->value() < 0 || s->value() > 1000)) ++bad;
            }
        });
    for (int i = 0; i < 2000; ++i) {
        auto s = std::make_shared<Alpha>(i % 1000);
        svcSet<IAlpha>(s);
        if (i % 3 == 0) svcRemoveIf<IAlpha>(s);
    }
    stop = true;
    for (auto& t : th) t.join();
    QCOMPARE(bad.load(), 0);
}

void TestAnfsCoreConfig::idsDigestVectors()
{
    SEQ(ids::md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    SEQ(ids::md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    SEQ(ids::md5Hex12("abc"), "900150983cd2");   // C# FsCoreTests vector
    // Agent ids: md5Hex12(hostname + "_" + ip) (core/ids.h examples).
    SEQ(ids::md5Hex12("PC1_192.168.1.20"), "cb0bff524238");
    SEQ(ids::md5Hex12("SERVER_192.168.1.177"), "d013d23dcf03");
    SEQ(ids::sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    SEQ(ids::sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    SEQ(ids::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    SEQ(ids::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    SEQ(hex::encode(ids::sha256("The quick brown fox jumps over the lazy dog")),
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    QCOMPARE(ids::sha256("x").size(), size_t(32));
    // RFC 4231 cases 1, 2 and 6 (key longer than a block), and the empty key and message.
    SEQ(hex::encode(ids::hmacSha256(std::string(20, '\x0b'), "Hi There")),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    SEQ(hex::encode(ids::hmacSha256("Jefe", "what do ya want for nothing?")),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    SEQ(hex::encode(ids::hmacSha256(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    SEQ(hex::encode(ids::hmacSha256("", "")), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");

    // Incremental digests over a million 'a' in uneven pieces (FIPS 180 / RFC 1321 long-message vectors).
    const std::string million(1000000, 'a');
    struct Want {
        ids::Hasher::Algo algo;
        const char* hex;
    };
    for (const Want& wnt : {Want{ids::Hasher::Algo::Sha256, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
                            Want{ids::Hasher::Algo::Md5, "7707d6ae4e027c70eea2a935c2296f21"},
                            Want{ids::Hasher::Algo::Sha1, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"}}) {
        ids::Hasher h(wnt.algo);
        size_t off = 0, step = 1;
        while (off < million.size()) {
            const size_t n = std::min(step, million.size() - off);
            h.update(million.data() + off, n);
            off += n;
            step = step * 3 + 7;
        }
        SEQ(h.finishHex(), wnt.hex);
        bool threw = false;
        try {
            h.update("x");
        } catch (const std::logic_error&) {
            threw = true;
        }
        QVERIFY(threw);   // a finished hasher cannot be reused
    }
    SEQ(ids::sha256Hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void TestAnfsCoreConfig::idsFormatsAndRandom()
{
    const QRegularExpression logRx("^log_\\d{13}_[a-z0-9]{4}$"), txRx("^tx_\\d{13}_[a-z0-9]{5}$"), shareRx("^share_\\d{13}$"),
        uuidRx("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"), zipRx("^zip_\\d{13}_[a-z0-9]{5}$");
    std::set<std::string> seenLog, seenUuid;
    for (int i = 0; i < 300; ++i) {
        const std::string l = ids::logId(), u = ids::uuid();
        QVERIFY2(logRx.match(QString::fromStdString(l)).hasMatch(), l.c_str());
        QVERIFY2(txRx.match(QString::fromStdString(ids::txId())).hasMatch(), "txId");
        QVERIFY(shareRx.match(QString::fromStdString(ids::shareId())).hasMatch());
        QVERIFY2(uuidRx.match(QString::fromStdString(u)).hasMatch(), u.c_str());
        QVERIFY(zipRx.match(QString::fromStdString(ids::prefixedId("zip", 5))).hasMatch());
        seenLog.insert(l);
        seenUuid.insert(u);
    }
    QVERIFY(seenLog.size() > 290);    // 4 base36 characters per millisecond: repeats are possible but rare
    QCOMPARE(seenUuid.size(), size_t(300));
    SEQ(ids::prefixedId("p", 0).substr(0, 2), "p_");
    QVERIFY(ids::prefixedId("p", 0).find('_', 2) == std::string::npos);

    // The time part comes from anfs::clock().
    {
        ManualClock mc(1760000000000LL);
        ScopedClock scope(mc);
        QVERIFY(ids::logId().rfind("log_1760000000000_", 0) == 0);
        SEQ(ids::shareId(), "share_1760000000000");
        mc.advance(5);
        QVERIFY(ids::txId().rfind("tx_1760000000005_", 0) == 0);
    }

    const std::string b36 = ids::randomBase36(36000);
    QCOMPARE(b36.size(), size_t(36000));
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    int counts[36] = {};
    for (char c : b36) {
        const char* p = std::strchr(alphabet, c);
        QVERIFY(p && c != '\0');
        ++counts[p - alphabet];
    }
    for (int n : counts) QVERIFY2(n > 700 && n < 1300, "base36 characters are roughly uniform");
    QCOMPARE(ids::randomHex(16).size(), size_t(32));
    QVERIFY(hex::isHex(ids::randomHex(16)));
    QVERIFY(ids::randomHex(0).empty());
    QVERIFY(ids::randomBase36(0).empty());
    std::vector<uint8_t> buf(4096, 0);
    ids::randomBytes(buf.data(), buf.size());
    QVERIFY(std::count(buf.begin(), buf.end(), uint8_t(0)) < 64);
}

void TestAnfsCoreConfig::hexBase64AndConstantTime()
{
    SEQ(hex::encode("\x00\x01\xab\xff", 4), "0001abff");
    SEQ(hex::ofText("f1_a b.txt"), "66315f6120622e747874");   // upload ids (core/ids.h)
    QVERIFY(hex::isHex("00aAfF") && !hex::isHex("") && !hex::isHex("abc") && !hex::isHex("0g"));
    const std::vector<uint8_t> want{0x00, 0x01, 0xab, 0xff};
    QVERIFY(hex::decode("0001ABff") == want);
    QVERIFY(hex::decode("zz").empty() && hex::decode("123").empty());

    // RFC 4648 section 10.
    const char* const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* const enc[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        SEQ(b64::encode(std::string_view(plain[i])), enc[i]);
        std::vector<uint8_t> out;
        QVERIFY(b64::decode(enc[i], out));
        QCOMPARE(std::string(out.begin(), out.end()), std::string(plain[i]));
    }
    const std::vector<uint8_t> fbff{0xfb, 0xff};
    SEQ(b64::encode(fbff), "+/8=");
    std::vector<uint8_t> out;
    QVERIFY(b64::decode(" Zm9v\r\nYmFy ", out) && std::string(out.begin(), out.end()) == "foobar");   // whitespace ignored
    QVERIFY(!b64::decode("Zm9", out) && out.empty());       // not a multiple of 4
    QVERIFY(!b64::decode("Zm=v", out));                     // padding in the middle
    QVERIFY(!b64::decode("Zg===", out));
    QVERIFY(!b64::decode("Zm9v!A==", out));
    QVERIFY(b64::decode("", out) && out.empty());

    QVERIFY(constantTimeEquals("abc", "abc"));
    QVERIFY(!constantTimeEquals("abc", "abd"));
    QVERIFY(!constantTimeEquals("abc", "abcd"));
    QVERIFY(!constantTimeEquals("", "a"));
    QVERIFY(constantTimeEquals("", ""));
    QVERIFY(constantTimeEquals(std::string(1000, 'k'), std::string(1000, 'k')));
    QVERIFY(!constantTimeEquals(std::string("a\0b", 3), std::string("a\0c", 3)));
}

void TestAnfsCoreConfig::clockSystemAndManual()
{
    // The process clock is the system clock unless a test installs another one.
    const int64_t sys = nowMs();
    QVERIFY(std::llabs(sys - QDateTime::currentMSecsSinceEpoch()) < 2000);
    const int64_t m1 = monoMs();
    QThread::msleep(20);
    QVERIFY(monoMs() >= m1 + 15);
    SystemClock sc;
    std::mutex mu;
    std::condition_variable cv;
    {
        std::unique_lock<std::mutex> lk(mu);
        QElapsedTimer t;
        t.start();
        QVERIFY(!sc.waitFor(cv, lk, 50, [] { return false; }));
        QVERIFY(t.elapsed() >= 40);
        QVERIFY(sc.waitFor(cv, lk, 0, [] { return true; }));
        QVERIFY(!sc.waitFor(cv, lk, -5, [] { return false; }));
    }
    bool flag = false;
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::lock_guard<std::mutex> g(mu);
        flag = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lk(mu);
        QElapsedTimer t;
        t.start();
        QVERIFY(sc.waitFor(cv, lk, INT64_MAX, [&] { return flag; }));   // "forever" does not overflow
        QVERIFY(t.elapsed() < 2000);
    }
    setter.join();

    ManualClock mc(1000);
    QCOMPARE(mc.nowMs(), int64_t(1000));
    QCOMPARE(mc.monoMs(), int64_t(0));
    mc.advance(250);
    QCOMPARE(mc.nowMs(), int64_t(1250));
    QCOMPARE(mc.monoMs(), int64_t(250));
    mc.set(5000);   // wall-clock jump: monotonic time unchanged
    QCOMPARE(mc.nowMs(), int64_t(5000));
    QCOMPARE(mc.monoMs(), int64_t(250));
    {
        ScopedClock scope(mc);
        QCOMPARE(nowMs(), int64_t(5000));
        QCOMPARE(&anfs::clock(), static_cast<Clock*>(&mc));   // qualified: ::clock() from <time.h> is visible too
    }
    QVERIFY(&anfs::clock() != static_cast<Clock*>(&mc));

    // A 25 s relay park ends as soon as the test advances 25 s, and not before.
    std::atomic<int> result{-1};
    std::thread waiter([&] {
        std::unique_lock<std::mutex> lk(mu);
        result = mc.waitFor(cv, lk, 25000, [] { return false; }) ? 1 : 0;
    });
    QThread::msleep(50);
    mc.advance(24999);
    QThread::msleep(80);
    QCOMPARE(result.load(), -1);
    mc.advance(1);
    QVERIFY(waitReal([&] { return result.load() != -1; }, 2000));
    QCOMPARE(result.load(), 0);
    waiter.join();

    // A satisfied predicate returns without any time passing; a notify from another thread is seen within a slice.
    {
        std::unique_lock<std::mutex> lk(mu);
        QVERIFY(mc.waitFor(cv, lk, 1000000, [] { return true; }));
        QVERIFY(!mc.waitFor(cv, lk, 0, [] { return false; }));
    }
    flag = false;
    std::thread setter2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::lock_guard<std::mutex> g(mu);
        flag = true;
    });
    {
        std::unique_lock<std::mutex> lk(mu);
        QVERIFY(mc.waitFor(cv, lk, 1000000, [&] { return flag; }));   // no notify needed: waiters poll
    }
    setter2.join();

    std::atomic<bool> slept{false};
    std::thread sleeper([&] {
        mc.sleepMs(1000);
        slept = true;
    });
    QThread::msleep(50);
    QVERIFY(!slept.load());
    mc.advance(1000);
    QVERIFY(waitReal([&] { return slept.load(); }, 2000));
    sleeper.join();
    mc.sleepMs(0);   // returns at once
}

QTEST_GUILESS_MAIN(TestAnfsCoreConfig)
#include "test_anfs_core_config.moc"
