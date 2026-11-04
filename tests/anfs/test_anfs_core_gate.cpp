// test_anfs_core_gate - ANFS-F6: network classes (core/net.cpp), the WAN gate and password records (core/gate.cpp)
// and the two CORS flavours (core/cors.cpp).
//
// - Classify matrix: loopback, 10/8, 172.16/12, 192.168/16, 100.64/10, fc00::/7, fe80::/10, WAN, IPv4-mapped, the
//   addresses just outside each range, unparsable text; IpAddr parse forms, toString, CIDR matches.
// - Trusted-proxy last hop (wan-gate.test.js forwarded-header cases), the LanOnly source rule, loopback hosts, URLs.
// - PBKDF2-SHA256 on BCrypt against the published vectors; records made by makePasswordRecord, by the C#
//   `anfs.exe hash-password` and by Node wan-gate.js makeRecord all verify in C++ (skipped when the oracle is
//   missing); the hex record form; tampered records; the 20,000,000 iteration ceiling.
// - Basic header parsing, the wan-gate.test.js gate cases replayed through Gate::decide with a ManualClock: 401 +
//   challenge, wrong / right password, username ignored, colon passwords, agent-exempt routes, requireTls, route
//   scoping (gated / ungated samples, HEAD as GET, case and trailing slash), 20 failures -> 429 + Retry-After 600
//   and the window, the 10 min credential cache, fail closed without a record.
// - The middleware on the F4 server with the F5 client: Node texts and headers, open routes, lockout over HTTP.
// - Peer CORS reflects the Origin (OPTIONS 200 "OK"); server CORS uses the allow-list (OPTIONS 204, D1).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "core/clock.h"
#include "core/config.h"
#include "core/cors.h"
#include "core/gate.h"
#include "core/http_client.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/net.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStandardPaths>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace anfs;
using namespace anfs::net;

#define SEQ(a, b) QCOMPARE(QString::fromUtf8(std::string(a).c_str()), QString::fromUtf8(std::string(b).c_str()))

namespace {

struct Wsa {
    Wsa() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { WSACleanup(); }
};

const char* const kPassword = "test-only-password";
const char* const kWrong = "test-only-wrong";
const char* const kToken = "test-agent-token-f6";   // lower-case on purpose: the secrets guard flags upper-case tokens
const char* const kWanIp = "203.0.113.7";
const char* const kCSharpExe = "D:/anfs-oracle/anfs.exe";
const char* const kWanGateJs = "D:/anfs-oracle/bridge/wan-gate.js";

std::string basic(std::string_view password, std::string_view user = "owner")
{
    return "Basic " + b64::encode(std::string(user) + ":" + std::string(password));
}

IpAddr ip(std::string_view s)
{
    std::optional<IpAddr> a = IpAddr::parse(s);
    return a ? *a : IpAddr::none();
}

// wan-gate.js FILE_ACCESS_ROUTES as gate rules (the srv-wan module seeds these in Wave 1).
const char* const kFileAccessRules[] = {
    "GET /api/drives", "GET /api/shared-roots", "GET /api/shortcuts", "POST /api/shortcuts",
    "DELETE /api/shortcuts/{id}", "GET /api/browse", "GET /api/browse-pick", "GET /api/walk", "GET /api/download",
    "GET /api/download-dir", "GET /api/preview-text", "GET /api/preview-media", "GET /api/thumbnail",
    "GET /api/swarm/download", "GET /api/media", "GET /api/media/{id}", "GET /api/media/{id}/stream",
    "GET /api/hash/sources", "GET /api/search", "GET /api/recent-files", "GET /api/dropbox", "GET /api/dropbox/{node}",
    "GET /api/shares", "POST /api/shares", "DELETE /api/shares/{id}", "POST /api/public-link",
    "POST /api/create-folder", "POST /api/upload/{op}",
};

std::shared_ptr<Config> makeConfig(std::vector<std::string> rules, int64_t iterations = 1000)
{
    auto c = std::make_shared<Config>();
    c->mode = Config::kModeServer;
    c->agentToken = kToken;
    c->wan.passwordRecord = makePasswordRecord(kPassword, iterations);
    c->wan.trustedProxies = {"127.0.0.1", "::1", "192.168.1.177"};
    for (const std::string& r : rules) {
        std::optional<GateRule> g = GateRule::parse(JVal(r));
        if (g) c->wan.gatedPaths.push_back(*g);
    }
    return c;
}

std::vector<std::string> fileAccessRules()
{
    return std::vector<std::string>(std::begin(kFileAccessRules), std::end(kFileAccessRules));
}

struct Sample {
    const char* method;
    const char* url;
};

const Sample kGatedSamples[] = {
    {"GET", "/api/drives"}, {"GET", "/api/drives?agentId=a1"}, {"GET", "/api/shared-roots"},
    {"GET", "/api/shortcuts"}, {"POST", "/api/shortcuts"}, {"DELETE", "/api/shortcuts/s1"},
    {"GET", "/api/browse?path=C%3A%5C"}, {"GET", "/api/browse-pick?path=C%3A%5C"}, {"GET", "/api/walk?path=D%3A%5C"},
    {"GET", "/api/download?path=D%3A%5Cx.mp4"}, {"GET", "/api/download-dir?path=D%3A%5Cx"},
    {"GET", "/api/preview-text?path=D%3A%5Cx.txt"}, {"GET", "/api/preview-media?path=D%3A%5Cx.mp4"},
    {"GET", "/api/thumbnail?hash=abc"}, {"GET", "/api/swarm/download?hash=abc&name=x"},
    {"GET", "/api/media"}, {"GET", "/api/media/abc123"}, {"GET", "/api/media/abc123/stream"},
    {"GET", "/api/hash/sources?hash=abc"}, {"GET", "/api/search?q=x"}, {"GET", "/api/recent-files"},
    {"GET", "/api/dropbox/local"}, {"GET", "/api/dropbox/node-1"},
    {"GET", "/api/shares"}, {"POST", "/api/shares"}, {"DELETE", "/api/shares/sh1"},
    {"POST", "/api/public-link"}, {"POST", "/api/create-folder"},
    {"POST", "/api/upload/init"}, {"POST", "/api/upload/chunk?agentId=a1"}, {"POST", "/api/upload/complete"},
};

const Sample kUngatedSamples[] = {
    {"POST", "/phone/incoming"}, {"POST", "/phone2/voice"}, {"POST", "/voice"}, {"POST", "/sms"},
    {"GET", "/provision/x.cfg"}, {"GET", "/auto1/"}, {"POST", "/api/announcements/play"},
    {"GET", "/api/recordings/1.wav"}, {"POST", "/api/dispatch"}, {"POST", "/api/telle/event"},
    {"GET", "/.well-known/acme-challenge/tok3n-abc"}, {"GET", "/files/0123456789abcdef"},
    {"GET", "/"}, {"GET", "/index.html"}, {"GET", "/dash"}, {"GET", "/setup/setup.html"}, {"GET", "/css/app.css"},
    {"GET", "/Dropbox/logo.png"}, {"GET", "/api/setup"}, {"GET", "/api/download/agent.js"},
    {"GET", "/api/download/peer-agent"}, {"GET", "/api/download/peer-exe"}, {"GET", "/api/nodes"},
    {"GET", "/api/agent/list"}, {"GET", "/api/dashboard"}, {"GET", "/api/transfers"}, {"POST", "/api/transfer"},
    {"POST", "/api/transfer-batch"}, {"GET", "/api/transfers/xfer_abc"}, {"POST", "/api/agent/register"},
    {"POST", "/api/watchdog/report"}, {"GET", "/api/relay/poll?agentId=a1"}, {"GET", "/api/relay/body/job-1"},
    {"POST", "/api/relay/response/job-1"}, {"GET", "/api/buckets"}, {"POST", "/api/buckets"},
    {"POST", "/portal-login"}, {"GET", "/ws"},
};

// Node near misses that the segment matcher also leaves ungated ("//api/browse" is gated here: empty segments are
// dropped, which errs on the gated side).
const Sample kNearMisses[] = {
    {"GET", "/api/browser"}, {"GET", "/api/browse-x"}, {"GET", "/api/downloads"}, {"GET", "/api/download/agent.js"},
    {"GET", "/api/download-dir/extra"}, {"GET", "/api/upload/init"}, {"POST", "/api/browse"},
    {"GET", "/api/media/abc/stream/x"}, {"GET", "/api/media/ingests/status"}, {"POST", "/api/media/ingests/refresh"},
    {"GET", "/files/api/browse"},
};

std::string pathOf(const char* url)
{
    std::string s(url);
    const size_t q = s.find('?');
    return q == std::string::npos ? s : s.substr(0, q);
}

// A gate fed by a swappable config.
struct GateFixture {
    std::shared_ptr<Config> cfg;
    Gate gate;
    explicit GateFixture(std::shared_ptr<Config> c) : cfg(std::move(c)), gate([this] { return std::shared_ptr<const Config>(cfg); }) {}
    // A WAN client that arrived through the loopback proxy (realClient of 127.0.0.1 + X-Forwarded-For).
    GateResult wan(std::string_view method, const char* url, std::string_view auth = {}, std::string_view agent = {},
                   bool exempt = false, bool secure = false)
    {
        const IpAddr client = realClient(ip("127.0.0.1"), kWanIp, cfg->wan.trustedProxies);
        return gate.decide(method, pathOf(url), client, auth, agent, exempt, secure);
    }
    GateResult from(std::string_view peer, std::string_view method = "GET", const char* url = "/api/browse")
    {
        return gate.decide(method, pathOf(url), ip(peer), {}, {}, false, false);
    }
};

// ---- an F4 server with the trusted-proxy client rule wired ------------------------------------------------------------

struct TestServer {
    explicit TestServer(std::shared_ptr<Config> c) : cfg(std::move(c)), server(router, [this] { return std::shared_ptr<const Config>(cfg); }, nullptr, options()) {}
    http::ServerOptions options()
    {
        http::ServerOptions o;
        o.resolveClient = [](http::Request& req, const Config& c) {
            const IpAddr client = realClient(ip(req.peerIp), req.headers.get("x-forwarded-for"), c.wan.trustedProxies);
            req.clientIp = client.toString();
            req.clientClass = classify(client);
        };
        return o;
    }
    void start() { server.start(0, false, "127.0.0.1"); }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(server.port()) + path; }
    http::HttpClient::Result fetch(std::string_view method, const std::string& path,
                                   std::initializer_list<std::pair<const char*, std::string>> headers = {},
                                   std::string_view body = {})
    {
        http::Headers h;
        for (const auto& [k, v] : headers) h.add(k, v);
        return client.fetch(method, url(path), h, body, 10000);
    }
    std::shared_ptr<Config> cfg;
    http::Router router;
    http::Server server;
    http::HttpClient client;
};

} // namespace

class TestAnfsCoreGate : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { wsa_ = std::make_unique<Wsa>(); }
    void cleanupTestCase() { wsa_.reset(); }

    void classifyMatrix();
    void addressParsingAndMatching();
    void trustedProxyLastHop();
    void lanSourceLoopbackHostsAndUrls();
    void localMachineHelpers();
    void pbkdf2VectorsAndRecords();
    void recordsFromCSharpAndNodeOracles();
    void basicAuthParsing();
    void gateDecideReplay();
    void gateLockoutAndCredentialCache();
    void gateMiddlewareOverHttp();
    void corsHeadersAndOrigins();
    void corsMiddlewareOverHttp();

private:
    std::unique_ptr<Wsa> wsa_;
};

// ---- net -------------------------------------------------------------------------------------------------------------

void TestAnfsCoreGate::classifyMatrix()
{
    struct Row { const char* text; NetClass cls; };
    const Row rows[] = {
        {"127.0.0.1", NetClass::Loopback}, {"127.1.2.3", NetClass::Loopback}, {"127.255.255.255", NetClass::Loopback},
        {"::1", NetClass::Loopback}, {"::ffff:127.0.0.1", NetClass::Loopback}, {"[::1]:8080", NetClass::Loopback},
        {"10.0.0.5", NetClass::Lan}, {"10.255.255.255", NetClass::Lan}, {"172.16.0.1", NetClass::Lan},
        {"172.31.255.254", NetClass::Lan}, {"192.168.1.20", NetClass::Lan}, {"::ffff:10.0.0.5", NetClass::Lan},
        {"::FFFF:192.168.1.1", NetClass::Lan}, {"fd00::1", NetClass::Lan}, {"fc00::1", NetClass::Lan},
        {"fe80::1%3", NetClass::Lan}, {"fe80::1", NetClass::Lan}, {"febf::1", NetClass::Lan},
        {"100.64.0.1", NetClass::Tailnet}, {"100.100.100.100", NetClass::Tailnet}, {"100.127.255.254", NetClass::Tailnet},
        {"100.64.0.5", NetClass::Tailnet},
        {"172.15.255.255", NetClass::Wan}, {"172.32.0.1", NetClass::Wan}, {"192.169.0.1", NetClass::Wan},
        {"11.0.0.1", NetClass::Wan}, {"9.255.255.255", NetClass::Wan}, {"100.63.255.255", NetClass::Wan},
        {"100.128.0.1", NetClass::Wan}, {"203.0.113.7", NetClass::Wan}, {"8.8.8.8", NetClass::Wan},
        {"::FFFF:203.0.113.7", NetClass::Wan}, {"2001:db8::5", NetClass::Wan}, {"fec0::1", NetClass::Wan},
        {"fb00::1", NetClass::Wan}, {"::", NetClass::Wan}, {"::2", NetClass::Wan},
        {"", NetClass::Wan}, {"not-an-ip", NetClass::Wan}, {"10.0.0", NetClass::Wan}, {"10.0.0.5.6", NetClass::Wan},
        {"localhost", NetClass::Wan}, {"256.1.1.1", NetClass::Wan},
    };
    for (const Row& r : rows) {
        const NetClass got = classify(std::string_view(r.text));
        QVERIFY2(got == r.cls, qPrintable(QString("%1: got %2").arg(r.text, className(got))));
    }
    SEQ(className(NetClass::Loopback), "Loopback");
    SEQ(className(NetClass::Lan), "Lan");
    SEQ(className(NetClass::Tailnet), "Tailnet");
    SEQ(className(NetClass::Wan), "Wan");
    QVERIFY(isLocalOrPrivate(NetClass::Tailnet));
    QVERIFY(!isLocalOrPrivate(NetClass::Wan));
    QCOMPARE(classify(IpAddr::none()), NetClass::Wan);
}

void TestAnfsCoreGate::addressParsingAndMatching()
{
    QVERIFY(IpAddr::parse("1.2.3.4"));
    SEQ(ip("1.2.3.4").toString(), "1.2.3.4");
    SEQ(ip("1.2.3.4:80").toString(), "1.2.3.4");
    SEQ(ip("[::1]:80").toString(), "::1");
    SEQ(ip(" \"192.168.1.177\" ").toString(), "192.168.1.177");
    SEQ(ip("fe80::1%3").toString(), "fe80::1");
    SEQ(ip("2001:DB8:0:0:0:0:0:5").toString(), "2001:db8::5");
    SEQ(ip("::ffff:1.2.3.4").toString(), "::ffff:1.2.3.4");
    QVERIFY(ip("::ffff:1.2.3.4").v6);
    QVERIFY(!ip("::ffff:1.2.3.4").normalized().v6);
    SEQ(ip("::ffff:1.2.3.4").normalized().toString(), "1.2.3.4");
    QVERIFY(ip("::ffff:1.2.3.4").normalized() == IpAddr::v4(1, 2, 3, 4));
    QVERIFY(ip("::ffff:1.2.3.4") != IpAddr::v4(1, 2, 3, 4));   // == compares as written; normalize first
    QVERIFY(ip("1.2.3.4") != ip("1.2.3.5"));
    QVERIFY(IpAddr::none() == IpAddr::none());
    QVERIFY(IpAddr::none().toString().empty());
    QVERIFY(ip("127.0.0.1").isLoopback());
    QVERIFY(ip("::ffff:127.4.4.4").isLoopback());
    QVERIFY(ip("::1").isLoopback());
    QVERIFY(!ip("10.0.0.1").isLoopback());
    for (const char* bad : {"", " ", "1.2.3", "1.2.3.4.5", "[::1", "a::b::c", "1.2.3.4:80:90", "::1:80", "256.0.0.1"}) {
        if (std::string(bad) == "::1:80") {
            QVERIFY(IpAddr::parse(bad));   // a valid IPv6 ("::1:80" = 0:0:0:0:0:0:1:80), not "::1 port 80"
            continue;
        }
        QVERIFY2(!IpAddr::parse(bad), bad);
    }

    QVERIFY(matches(ip("10.1.2.3"), "10.0.0.0/8"));
    QVERIFY(!matches(ip("11.1.2.3"), "10.0.0.0/8"));
    QVERIFY(matches(ip("172.31.0.1"), "172.16.0.0/12"));
    QVERIFY(!matches(ip("172.32.0.1"), "172.16.0.0/12"));
    QVERIFY(matches(ip("192.168.1.177"), "192.168.1.177"));
    QVERIFY(matches(ip("::ffff:192.168.1.177"), "192.168.1.177"));
    QVERIFY(matches(ip("192.168.1.177"), " 192.168.1.177 "));
    QVERIFY(matches(ip("192.168.1.177"), "192.168.1.177/32"));
    QVERIFY(!matches(ip("10.0.0.178"), "192.168.1.177/32"));
    QVERIFY(matches(ip("10.0.0.178"), "0.0.0.0/0"));
    QVERIFY(matches(ip("fd12::1"), "fd00::/8"));
    QVERIFY(!matches(ip("fe12::1"), "fd00::/8"));
    QVERIFY(!matches(ip("10.0.0.1"), "fd00::/8"));            // family mismatch
    QVERIFY(!matches(ip("10.0.0.1"), "10.0.0.0/33"));
    QVERIFY(!matches(ip("10.0.0.1"), "10.0.0.0/"));
    QVERIFY(!matches(ip("10.0.0.1"), "10.0.0.0/x"));
    QVERIFY(!matches(ip("10.0.0.1"), ""));
    QVERIFY(!matches(IpAddr::none(), "0.0.0.0/0"));
    const std::vector<std::string> trusted{"127.0.0.1", "::1", "192.168.1.177"};
    QVERIFY(isTrustedProxy(ip("127.0.0.1"), trusted));
    QVERIFY(isTrustedProxy(ip("::1"), trusted));
    QVERIFY(isTrustedProxy(ip("::ffff:127.0.0.1"), trusted));
    QVERIFY(!isTrustedProxy(ip("127.0.0.2"), trusted));
    QVERIFY(!isTrustedProxy(ip("10.0.0.178"), trusted));
    QVERIFY(!isTrustedProxy(IpAddr::none(), trusted));
    QVERIFY(!isTrustedProxy(ip("127.0.0.1"), {}));
}

void TestAnfsCoreGate::trustedProxyLastHop()
{
    const std::vector<std::string> trusted{"127.0.0.1", "::1", "192.168.1.177"};
    // untrusted peers: the header is ignored
    SEQ(realClient(ip(kWanIp), "10.0.0.5", trusted).toString(), kWanIp);
    SEQ(realClient(ip("10.0.0.50"), kWanIp, trusted).toString(), "10.0.0.50");
    QCOMPARE(classify(realClient(ip("10.0.0.50"), kWanIp, trusted)), NetClass::Lan);
    // trusted peers honour the header
    for (const char* peer : {"127.0.0.1", "::1", "192.168.1.177", "::ffff:127.0.0.1"})
        SEQ(realClient(ip(peer), kWanIp, trusted).toString(), kWanIp);
    SEQ(realClient(ip("10.0.0.178"), kWanIp, trusted).toString(), "10.0.0.178");
    // the LAST entry is the client
    const IpAddr spoofedLan = realClient(ip("127.0.0.1"), std::string("10.0.0.5, ") + kWanIp, trusted);
    SEQ(spoofedLan.toString(), kWanIp);
    QCOMPARE(classify(spoofedLan), NetClass::Wan);
    const IpAddr honest = realClient(ip("127.0.0.1"), std::string(kWanIp) + ", 10.0.0.5", trusted);
    SEQ(honest.toString(), "10.0.0.5");
    QCOMPARE(classify(honest), NetClass::Lan);
    SEQ(realClient(ip("127.0.0.1"), "1.1.1.1,2.2.2.2,  3.3.3.3  ", trusted).toString(), "3.3.3.3");
    // an unparsable last entry fails closed
    for (const char* xff : {"10.0.0.5,", "not-an-ip", "10.0.0.5, unknown"}) {
        const IpAddr c = realClient(ip("127.0.0.1"), xff, trusted);
        QVERIFY2(!c.valid, xff);
        QCOMPARE(classify(c), NetClass::Wan);
    }
    // no header: the peer is the client (C# rule; blank counts as absent)
    QCOMPARE(classify(realClient(ip("127.0.0.1"), "", trusted)), NetClass::Loopback);
    QCOMPARE(classify(realClient(ip("127.0.0.1"), "   ", trusted)), NetClass::Loopback);
    QCOMPARE(classify(realClient(ip("192.168.1.177"), "", trusted)), NetClass::Lan);
    // a mapped header value is normalised; an IPv6 client with a port form
    SEQ(realClient(ip("127.0.0.1"), "::ffff:10.0.0.5", trusted).toString(), "10.0.0.5");
    SEQ(realClient(ip("127.0.0.1"), "[2001:db8::5]:443", trusted).toString(), "2001:db8::5");
    // an empty trusted list never honours the header (Node trustForwarded:false)
    QCOMPARE(classify(realClient(ip("127.0.0.1"), kWanIp, {})), NetClass::Loopback);
    // an invalid peer stays invalid
    QVERIFY(!realClient(IpAddr::none(), "10.0.0.5", trusted).valid);
    // CIDR entries in the trusted list
    SEQ(realClient(ip("10.0.0.9"), kWanIp, {"10.0.0.0/24"}).toString(), kWanIp);
    SEQ(realClient(ip("10.0.1.9"), kWanIp, {"10.0.0.0/24"}).toString(), "10.0.1.9");
}

void TestAnfsCoreGate::lanSourceLoopbackHostsAndUrls()
{
    for (const char* ok : {"127.0.0.1", "127.9.9.9", "10.1.1.1", "172.16.0.1", "172.31.255.255", "192.168.0.1",
                           "169.254.1.1", "::1", "::ffff:192.168.1.1"})
        QVERIFY2(isLanSource(ip(ok)), ok);
    for (const char* no : {"100.64.0.1", "100.64.0.5", "172.32.0.1", "8.8.8.8", "fd00::1", "fe80::1", "", "x"})
        QVERIFY2(!isLanSource(ip(no)), no);

    for (const char* ok : {"localhost", "LOCALHOST", "127.0.0.1", "127.5.5.5", "::1", "[::1]", "::ffff:127.0.0.1", " localhost "})
        QVERIFY2(isLoopbackHost(ok), ok);
    for (const char* no : {"wyat.getstarnews.com", "192.168.1.177", "localhost.evil", "127.0.0.1.evil", "", "::2", "0.0.0.0"})
        QVERIFY2(!isLoopbackHost(no), no);

    std::optional<Url> u = parseUrl("http://192.168.1.177:8080/api/agent/register?x=1#frag");
    QVERIFY(u);
    SEQ(u->scheme, "http");
    SEQ(u->host, "192.168.1.177");
    QCOMPARE(u->port, 8080);
    SEQ(u->pathAndQuery, "/api/agent/register?x=1");
    SEQ(u->origin(), "http://192.168.1.177:8080");
    u = parseUrl("HTTPS://Wyat.GetStarNews.com");
    QVERIFY(u);
    SEQ(u->scheme, "https");
    SEQ(u->host, "wyat.getstarnews.com");
    QCOMPARE(u->port, 443);
    SEQ(u->pathAndQuery, "/");
    SEQ(u->origin(), "https://wyat.getstarnews.com");
    u = parseUrl("http://wyat.getstarnews.com:80/");
    QVERIFY(u);
    SEQ(u->origin(), "http://wyat.getstarnews.com");          // default port omitted
    u = parseUrl("https://wyat.getstarnews.com:80?q");
    QVERIFY(u);
    SEQ(u->origin(), "https://wyat.getstarnews.com:80");
    SEQ(u->pathAndQuery, "/?q");
    u = parseUrl("http://[::1]:18080/x");
    QVERIFY(u);
    SEQ(u->host, "::1");
    QCOMPARE(u->port, 18080);
    SEQ(u->origin(), "http://[::1]:18080");
    u = parseUrl("http://[fe80::1]/");
    QVERIFY(u);
    SEQ(u->origin(), "http://[fe80::1]");
    for (const char* bad : {"", "null", "wyat.getstarnews.com", "file://", "ftp://wyat.getstarnews.com", "http://",
                            "http:///x", "http://host:0/", "http://host:65536/", "http://host:12a/", "http://[::1/",
                            "http://[nope]/", "http://::1/", "http://a b/", "http://user@host/", "://x"})
        QVERIFY2(!parseUrl(bad), bad);
}

void TestAnfsCoreGate::localMachineHelpers()
{
    const std::string host = hostName();
    QVERIFY(!host.empty());
    QVERIFY(host.find(' ') == std::string::npos);
    std::optional<std::string> local = localAddressFor("127.0.0.1", 9);
    QVERIFY(local);
    SEQ(*local, "127.0.0.1");
    local = localAddressFor("localhost", 18080);
    QVERIFY(local);
    QVERIFY(ip(*local).isLoopback());
    QVERIFY(!localAddressFor("", 9));
    QVERIFY(!localAddressFor("no-such-host.invalid", 9));
    const std::vector<std::string> v4 = localIPv4Addresses();
    for (const std::string& a : v4) {
        QVERIFY2(IpAddr::parse(a) && !ip(a).v6, a.c_str());
        QVERIFY2(!ip(a).isLoopback(), a.c_str());
    }
    // the route to a LAN host, when the machine has one, is one of the listed addresses
    if (!v4.empty()) {
        std::optional<std::string> viaLan = localAddressFor("192.168.1.177", 8080);
        if (viaLan) QVERIFY2(std::find(v4.begin(), v4.end(), *viaLan) != v4.end(), viaLan->c_str());
    }
}

// ---- records ---------------------------------------------------------------------------------------------------------

void TestAnfsCoreGate::pbkdf2VectorsAndRecords()
{
    // PBKDF2-HMAC-SHA256 published vectors (RFC 6070 inputs with SHA-256; the IETF draft-josefsson values).
    const std::vector<uint8_t> salt{'s', 'a', 'l', 't'};
    SEQ(hex::encode(pbkdf2Sha256("password", salt, 1, 32)), "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    SEQ(hex::encode(pbkdf2Sha256("password", salt, 2, 32)), "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    SEQ(hex::encode(pbkdf2Sha256("password", salt, 4096, 32)), "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    SEQ(hex::encode(pbkdf2Sha256("password", salt, 1, 20)), "120fb6cffcf8b32c43e7225256c4f837a86548c9");
    const std::vector<uint8_t> longSalt{'s', 'a', 'l', 't', 'S', 'A', 'L', 'T', 's', 'a', 'l', 't', 'S', 'A', 'L', 'T',
                                        's', 'a', 'l', 't', 'S', 'A', 'L', 'T', 's', 'a', 'l', 't', 'S', 'A', 'L', 'T',
                                        's', 'a', 'l', 't'};
    SEQ(hex::encode(pbkdf2Sha256("passwordPASSWORDpassword", longSalt, 4096, 40)),
        "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    QVERIFY(pbkdf2Sha256("", salt, 1, 32) != pbkdf2Sha256("x", salt, 1, 32));   // an empty password derives too

    // records
    const PasswordRecord rec = makePasswordRecord(kPassword, 1000);
    SEQ(rec.algo, "pbkdf2-sha256");
    QCOMPARE(rec.iterations, int64_t(1000));
    std::vector<uint8_t> saltBytes, hashBytes;
    QVERIFY(b64::decode(rec.salt, saltBytes) && saltBytes.size() == 16);
    QVERIFY(b64::decode(rec.hash, hashBytes) && hashBytes.size() == 32);
    QVERIFY(rec.isSet());
    QVERIFY(verifyPassword(rec, kPassword));
    QVERIFY(!verifyPassword(rec, kWrong));
    QVERIFY(!verifyPassword(rec, ""));
    QVERIFY(!verifyPassword(rec, std::string(kPassword) + " "));
    QVERIFY(makePasswordRecord(kPassword, 1000).salt != rec.salt);   // a fresh salt each time
    const std::string json = json::stringify(rec.toJval());
    QVERIFY(json.find(kPassword) == std::string::npos);
    SEQ(json.substr(0, 9), "{\"algo\":\"");
    QVERIFY(json.find("\"iterations\":1000,\"salt\":\"") != std::string::npos);
    QCOMPARE(makePasswordRecord("x").iterations, int64_t(600000));
    QCOMPARE(makePasswordRecord("x", 0).iterations, int64_t(600000));

    // tampered records
    PasswordRecord t = rec;
    hashBytes[0] ^= 0xFF;
    t.hash = b64::encode(hashBytes);
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.salt = makePasswordRecord(kPassword, 1000).salt;
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.iterations = 1001;
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.algo = "md5";
    QVERIFY(!t.isSet() && !verifyPassword(t, kPassword));
    t = rec; t.hash.clear();
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.hash = "not base64!";
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.salt = "AA";                                   // bad base64 length
    QVERIFY(!verifyPassword(t, kPassword));
    t = rec; t.iterations = 20000001;                         // ceiling: refused without deriving
    QVERIFY(t.isSet() && !verifyPassword(t, kPassword));
    t = rec; t.iterations = 0;
    QVERIFY(!verifyPassword(t, kPassword));
    QVERIFY(!verifyPassword(PasswordRecord{}, kPassword));
    // algo spellings the config accepts
    t = rec; t.algo = "PBKDF2_SHA256";
    QVERIFY(verifyPassword(t, kPassword));
    t = rec; t.algo = "pbkdf2";
    QVERIFY(verifyPassword(t, kPassword));

    // the hex form: a 64-hex hash with a hex salt
    const std::vector<uint8_t> hs = hex::decode("00112233445566778899aabbccddeeff");
    PasswordRecord hx;
    hx.algo = "pbkdf2-sha256";
    hx.iterations = 1000;
    hx.salt = "00112233445566778899aabbccddeeff";
    hx.hash = hex::encode(pbkdf2Sha256(kPassword, hs, 1000, 32));
    QVERIFY(verifyPassword(hx, kPassword));
    QVERIFY(!verifyPassword(hx, kWrong));
    hx.hash[0] = hx.hash[0] == 'f' ? '0' : 'f';
    QVERIFY(!verifyPassword(hx, kPassword));
    // a 64-char base64 hash (48 bytes) with a base64 salt is NOT the hex form
    PasswordRecord b48;
    b48.algo = "pbkdf2-sha256";
    b48.iterations = 1000;
    b48.salt = b64::encode(hs);
    b48.hash = b64::encode(pbkdf2Sha256(kPassword, hs, 1000, 48));
    QCOMPARE(b48.hash.size(), size_t(64));
    QVERIFY(verifyPassword(b48, kPassword));
}

void TestAnfsCoreGate::recordsFromCSharpAndNodeOracles()
{
    bool ranOne = false;
    // C#: anfs.exe hash-password reads ANFS_PASSWORD and prints the record (Program.cs HashPassword).
    if (QFileInfo::exists(kCSharpExe)) {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANFS_PASSWORD", kPassword);
        p.setProcessEnvironment(env);
        p.start(kCSharpExe, {"hash-password", "--iterations", "1000"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        const std::string out = p.readAllStandardOutput().toStdString();
        QVERIFY(out.find(kPassword) == std::string::npos);
        std::optional<PasswordRecord> rec = PasswordRecord::fromJval(json::parse(out));
        QVERIFY(rec);
        SEQ(rec->algo, "pbkdf2-sha256");
        QCOMPARE(rec->iterations, int64_t(1000));
        QVERIFY(rec->isSet());
        QVERIFY(verifyPassword(*rec, kPassword));
        QVERIFY(!verifyPassword(*rec, kWrong));
        // and the C# tool refuses a short password without echoing it
        env.insert("ANFS_PASSWORD", "ab12");
        p.setProcessEnvironment(env);
        p.start(kCSharpExe, {"hash-password"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 2);
        QVERIFY(p.readAllStandardOutput().isEmpty());
        QVERIFY(p.readAllStandardError().toStdString().find("ab12") == std::string::npos);
        ranOne = true;
    } else {
        qWarning("C# oracle %s not found: skipping that half", kCSharpExe);
    }
    // Node: wan-gate.js makeRecord (crypto.pbkdf2Sync) and checkBasic on a C++ record.
    const QString node = QStandardPaths::findExecutable("node");
    if (!node.isEmpty() && QFileInfo::exists(kWanGateJs)) {
        const PasswordRecord mine = makePasswordRecord(kPassword, 1000);
        const QString script = QString(
            "const g=require(%1);const r=g.makeRecord(process.env.ANFS_PASSWORD,1000);"
            "const mine=JSON.parse(process.env.ANFS_MINE);"
            "const b=(p)=>'Basic '+Buffer.from('owner:'+p).toString('base64');"
            "(async()=>{const ok=await g.checkBasic(b(process.env.ANFS_PASSWORD),mine);"
            "const bad=await g.checkBasic(b('test-only-wrong'),mine);"
            "process.stdout.write(JSON.stringify({record:r,ok,bad}));})();")
            .arg(QString::fromUtf8(json::quote(kWanGateJs).c_str()));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANFS_PASSWORD", kPassword);
        env.insert("ANFS_MINE", QString::fromUtf8(json::stringify(mine.toJval()).c_str()));
        p.setProcessEnvironment(env);
        p.setWorkingDirectory(QFileInfo(kWanGateJs).absolutePath());
        p.start(node, {"-e", script});
        QVERIFY(p.waitForFinished(30000));
        QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
        const JVal out = json::parse(p.readAllStandardOutput().toStdString());
        std::optional<PasswordRecord> rec = PasswordRecord::fromJval(out.get("record"));
        QVERIFY(rec);
        QVERIFY(verifyPassword(*rec, kPassword));
        QVERIFY(!verifyPassword(*rec, kWrong));
        QVERIFY(out.get("ok").isBool() && out.get("ok").asBool());
        QVERIFY(out.get("bad").isBool() && !out.get("bad").asBool());
        ranOne = true;
    } else {
        qWarning("node or %s not found: skipping that half", kWanGateJs);
    }
    if (!ranOne) QSKIP("neither the C# anfs.exe nor node + wan-gate.js is available on this machine");
}

void TestAnfsCoreGate::basicAuthParsing()
{
    std::string pw;
    QVERIFY(basicPassword(basic(kPassword), pw));
    SEQ(pw, kPassword);
    QVERIFY(basicPassword(basic("pa:ss:word-long", "u"), pw));
    SEQ(pw, "pa:ss:word-long");
    QVERIFY(basicPassword(basic("", "u"), pw));
    SEQ(pw, "");
    QVERIFY(basicPassword("basic " + b64::encode(":onlypw"), pw));   // case-insensitive scheme, empty user
    SEQ(pw, "onlypw");
    QVERIFY(basicPassword("  Basic   " + b64::encode("a:b") + "  ", pw));
    SEQ(pw, "b");
    QVERIFY(basicPassword("Basic " + b64::encode("\xC3\xA9:p\xC3\xA4ss"), pw));   // UTF-8 bytes kept
    SEQ(pw, "p\xC3\xA4ss");
    for (const std::string& bad : {std::string("Bearer ") + kPassword, std::string("Basic !!!"),
                                   "Basic " + b64::encode("nocolon"), std::string(""), std::string("Basic"),
                                   std::string("Basic "), std::string("Basicx " + b64::encode("a:b")),
                                   std::string("Digest " + b64::encode("a:b"))}) {
        pw = "untouched";
        QVERIFY2(!basicPassword(bad, pw), bad.c_str());
        SEQ(pw, "untouched");
    }
}

// ---- gate ------------------------------------------------------------------------------------------------------------

void TestAnfsCoreGate::gateDecideReplay()
{
    ManualClock clk;
    ScopedClock installed(clk);
    GateFixture f(makeConfig(fileAccessRules()));

    // classes pass without a password
    for (const char* peer : {"10.0.0.5", "10.255.255.255", "172.16.0.1", "172.31.255.254", "192.168.1.20",
                             "100.64.0.1", "100.100.100.100", "100.127.255.254", "127.0.0.1", "127.1.2.3", "::1",
                             "::ffff:127.0.0.1", "fd00::1"}) {
        const GateResult r = f.from(peer);
        QVERIFY2(r.allowed && r.reason == "local", peer);
    }
    // WAN without credentials: 401 with a challenge
    for (const GateResult& r : {f.wan("GET", "/api/browse"), f.gate.decide("GET", "/api/browse", ip(kWanIp), {}, {}, false, false)}) {
        QVERIFY(!r.allowed);
        QCOMPARE(r.status, 401);
        QVERIFY(r.challenge);
        QCOMPARE(r.retryAfterSec, 0);
    }
    // wrong / right / username ignored
    QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    QVERIFY(f.wan("GET", "/api/browse", basic(kWrong)).challenge);
    GateResult ok = f.wan("GET", "/api/browse", basic(kPassword));
    QVERIFY(ok.allowed);
    SEQ(ok.reason, "password");
    QVERIFY(!ok.viaCache && !ok.viaAgentToken);
    QVERIFY(f.wan("GET", "/api/browse", basic(kPassword, "anyone")).allowed);
    QVERIFY(!f.wan("GET", "/api/browse", basic(kWrong, "owner")).allowed);
    // malformed or non-Basic
    for (const std::string& auth : {std::string("Bearer ") + kPassword, std::string("Basic !!!"),
                                    "Basic " + b64::encode("nocolon"), std::string("")}) {
        const GateResult r = f.wan("GET", "/api/browse", auth);
        QVERIFY2(r.status == 401 && r.challenge, auth.c_str());
        SEQ(r.reason, "authentication required");
    }
    // colon passwords
    {
        GateFixture g(makeConfig(fileAccessRules()));
        g.cfg->wan.passwordRecord = makePasswordRecord("pa:ss:word-long", 1000);
        QVERIFY(g.wan("GET", "/api/browse", basic("pa:ss:word-long")).allowed);
        QVERIFY(!g.wan("GET", "/api/browse", basic("pa")).allowed);
    }
    // spoofed LAN via the first X-Forwarded-For hop is still WAN
    {
        const IpAddr c = realClient(ip("127.0.0.1"), std::string("10.0.0.5, ") + kWanIp, f.cfg->wan.trustedProxies);
        const GateResult r = f.gate.decide("GET", "/api/browse", c, {}, {}, false, false);
        QVERIFY(!r.allowed && r.status == 401);
    }
    // the invalid (fail-closed) client is WAN and keyed as "?"
    QCOMPARE(f.gate.decide("GET", "/api/browse", IpAddr::none(), {}, {}, false, false).status, 401);

    // no record: fail closed for WAN, LAN still passes
    {
        GateFixture g(makeConfig(fileAccessRules()));
        g.cfg->wan.passwordRecord.reset();
        QVERIFY(g.from("10.0.0.5").allowed);
        const GateResult r = g.wan("GET", "/api/browse", basic(kPassword));
        QVERIFY(!r.allowed && r.status == 401 && r.challenge);
        SEQ(r.reason, "no WAN password configured");
        g.cfg->wan.passwordRecord = PasswordRecord{};            // present but empty = not set
        QCOMPARE(g.wan("GET", "/api/browse", basic(kPassword)).status, 401);
        g.cfg->wan.passwordRecord = makePasswordRecord(kPassword, 1000);   // a config change applies at once
        QVERIFY(g.wan("GET", "/api/browse", basic(kPassword)).allowed);
    }
    // requireTls: 403 over plain HTTP even with the password or the token; secure passes; LAN passes; only gated
    {
        GateFixture g(makeConfig(fileAccessRules()));
        g.cfg->wan.requireTls = true;
        const GateResult plain = g.wan("GET", "/api/browse", basic(kPassword));
        QCOMPARE(plain.status, 403);
        QVERIFY(!plain.challenge);
        SEQ(plain.reason, "TLS required");
        QCOMPARE(g.wan("GET", "/api/browse-pick", {}, kToken, true).status, 403);
        QVERIFY(g.wan("GET", "/api/browse", basic(kPassword), {}, false, true).allowed);
        QVERIFY(g.from("10.0.0.5").allowed);
        QVERIFY(g.wan("POST", "/phone/incoming").allowed);
        QCOMPARE(g.wan("GET", "/api/browse").status, 403);
    }

    // agent routes: every path gated, the token passes only on exempt routes, only by header
    {
        GateFixture g(makeConfig({"* /*"}));
        for (const Sample& s : kGatedSamples) {
            const GateResult r = g.wan(s.method, s.url, {}, kToken, true);
            QVERIFY2(r.allowed && r.viaAgentToken && r.reason == "agent token", s.url);
            QVERIFY2(g.wan(s.method, s.url, {}, "wrong-token", true).status == 401, s.url);
            QVERIFY2(g.wan(s.method, s.url, {}, {}, true).status == 401, s.url);
            QVERIFY2(g.wan(s.method, s.url, {}, kToken, false).status == 401, s.url);   // not exempt
        }
        QVERIFY(g.wan("GET", "/api/relay/poll?agentId=x", {}, kToken, true).allowed);
        QCOMPARE(g.wan("GET", (std::string("/api/relay/poll?auth=") + kToken).c_str(), {}, {}, true).status, 401);
        QCOMPARE(g.wan("GET", "/anything/at/all").status, 401);
        g.cfg->agentToken.clear();                                         // no token configured: never exempt
        QCOMPARE(g.wan("GET", "/api/browse-pick", {}, kToken, true).status, 401);
        QCOMPARE(g.wan("GET", "/api/browse-pick", {}, "", true).status, 401);
    }
    // route scoping with the file-access list
    for (const Sample& s : kGatedSamples) {
        f.gate.reset();                                        // Node ran this with maxFailures: 1000
        QVERIFY2(Gate::isGated(*f.cfg, s.method, pathOf(s.url)), s.url);
        const GateResult none = f.wan(s.method, s.url);
        QVERIFY2(none.status == 401 && none.challenge, s.url);
        QVERIFY2(f.wan(s.method, s.url, basic(kWrong)).status == 401, s.url);
        QVERIFY2(f.wan(s.method, s.url, basic(kPassword)).allowed, s.url);
    }
    for (const Sample& s : kUngatedSamples) {
        QVERIFY2(!Gate::isGated(*f.cfg, s.method, pathOf(s.url)), s.url);
        const GateResult r = f.wan(s.method, s.url);
        QVERIFY2(r.allowed && r.reason == "not gated", s.url);
    }
    for (const Sample& s : kNearMisses) QVERIFY2(!Gate::isGated(*f.cfg, s.method, pathOf(s.url)), s.url);
    QVERIFY(f.wan("GET", "/files/abc", basic(kWrong)).allowed);
    QVERIFY(f.wan("POST", "/api/agent/register", {}, kToken, true).allowed);
    QVERIFY(f.wan("GET", "/api/relay/poll?agentId=a", {}, kToken, true).allowed);
    // browse-pick is gated for WAN, the token still passes it (exempt), not /api/browse
    QCOMPARE(f.wan("GET", "/api/browse-pick?path=C%3A%5C").status, 401);
    QCOMPARE(f.wan("GET", "/api/browse-pick", {}, "wrong", true).status, 401);
    QVERIFY(f.wan("GET", "/api/browse-pick?path=C%3A%5C", {}, kToken, true).allowed);
    QCOMPARE(f.wan("GET", "/api/browse", {}, kToken, false).status, 401);
    // every class passes every sample, also via a trusted proxy
    for (const char* peer : {"10.0.0.5", "192.168.1.9", "172.20.0.3", "127.0.0.1", "::1", "100.64.0.5", "100.64.0.9"}) {
        for (const Sample& s : kGatedSamples) QVERIFY2(f.from(peer, s.method, s.url).allowed, s.url);
        for (const Sample& s : kUngatedSamples) QVERIFY2(f.from(peer, s.method, s.url).allowed, s.url);
    }
    for (const char* via : {"10.0.0.5", "100.64.0.5"}) {
        const IpAddr c = realClient(ip("127.0.0.1"), via, f.cfg->wan.trustedProxies);
        for (const Sample& s : kGatedSamples)
            QVERIFY2(f.gate.decide(s.method, pathOf(s.url), c, {}, {}, false, false).allowed, s.url);
    }
    // case, trailing slash, HEAD as GET, a query left in the path
    for (const Sample& s : {Sample{"GET", "/API/Browse"}, Sample{"GET", "/api/browse/"}, Sample{"GET", "/api/browse/?path=x"},
                            Sample{"HEAD", "/api/download?path=x"}, Sample{"GET", "/api/media/abc/STREAM/"},
                            Sample{"HEAD", "/api/media/abc/stream"}}) {
        QVERIFY2(Gate::isGated(*f.cfg, s.method, pathOf(s.url)), s.url);
        QVERIFY2(f.gate.decide(s.method, s.url, ip(kWanIp), {}, {}, false, false).status == 401, s.url);
    }
    QVERIFY(!Gate::isGated(*f.cfg, "HEAD", "/api/shares/x"));      // HEAD maps to GET only, not DELETE
    QVERIFY(!Gate::isGated(*f.cfg, "OPTIONS", "/api/browse"));
    // a comma method list and an object rule
    {
        GateFixture g(makeConfig({"GET,POST /api/x", "* /api/y/{*rest}"}));
        QVERIFY(Gate::isGated(*g.cfg, "GET", "/api/x") && Gate::isGated(*g.cfg, "POST", "/api/x"));
        QVERIFY(!Gate::isGated(*g.cfg, "DELETE", "/api/x"));
        QVERIFY(Gate::isGated(*g.cfg, "DELETE", "/api/y/a/b") && Gate::isGated(*g.cfg, "GET", "/api/y"));
        QVERIFY(!Gate::isGated(*g.cfg, "GET", "/api/z"));
    }
    // no config at all fails closed
    {
        Gate g([] { return std::shared_ptr<const Config>(); });
        QCOMPARE(g.decide("GET", "/x", ip(kWanIp), basic(kPassword), {}, false, false).status, 401);
        QVERIFY(g.decide("GET", "/x", ip("10.0.0.1"), {}, {}, false, false).allowed);
    }
}

void TestAnfsCoreGate::gateLockoutAndCredentialCache()
{
    ManualClock clk;
    ScopedClock installed(clk);
    GateFixture f(makeConfig(fileAccessRules()));

    // a successful header is cached: the second decision does not derive
    const std::string header = basic(kPassword);
    QVERIFY(!f.wan("GET", "/api/browse", header).viaCache);
    GateResult again = f.wan("GET", "/api/browse", header);
    QVERIFY(again.allowed && again.viaCache);
    SEQ(again.reason, "cached");
    // the cache is per client: another WAN client with the same header derives again
    QVERIFY(!f.gate.decide("GET", "/api/browse", ip("198.51.100.9"), header, {}, false, false).viaCache);
    QVERIFY(f.gate.decide("GET", "/api/browse", ip("198.51.100.9"), header, {}, false, false).viaCache);
    // a different user name is a different header: derived once more, then cached
    QVERIFY(!f.wan("GET", "/api/browse", basic(kPassword, "other")).viaCache);
    QVERIFY(f.wan("GET", "/api/browse", basic(kPassword, "other")).viaCache);
    // the cache holds for 10 minutes, then the password is derived again (and cached again)
    clk.advance(Gate::kCacheMs - 1);
    QVERIFY(f.wan("GET", "/api/browse", header).viaCache);
    clk.advance(2);
    QVERIFY(!f.wan("GET", "/api/browse", header).viaCache);
    QVERIFY(f.wan("GET", "/api/browse", header).viaCache);
    // a password change invalidates nothing in the cache until it expires (the header is still the old one) but
    // reset() drops it
    f.gate.reset();
    QVERIFY(!f.wan("GET", "/api/browse", header).viaCache);

    // 19 failures: still 401; the 20th locks the client out for 10 minutes
    f.gate.reset();
    for (int i = 0; i < Gate::kMaxFailures - 1; ++i) {
        const GateResult r = f.wan("GET", "/api/browse", basic(kWrong));
        QVERIFY2(r.status == 401 && r.challenge && r.reason == "invalid credentials", qPrintable(QString::number(i)));
        QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));
    }
    QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    QVERIFY(f.gate.isLockedOut(ip(kWanIp)));
    GateResult locked = f.wan("GET", "/api/browse", basic(kPassword));   // the right password is refused too
    QCOMPARE(locked.status, 429);
    QVERIFY(!locked.challenge);
    QCOMPARE(locked.retryAfterSec, 600);
    SEQ(locked.reason, "too many failed attempts");
    QCOMPARE(f.wan("GET", "/api/browse").status, 429);                      // and no credentials at all
    clk.advance(90 * 1000 + 1);
    QCOMPARE(f.wan("GET", "/api/browse", basic(kPassword)).retryAfterSec, 510);   // ceil((600000 - 90001) / 1000)
    // other clients and ungated / non-WAN requests are unaffected
    QVERIFY(f.gate.decide("GET", "/api/browse", ip("198.51.100.9"), header, {}, false, false).allowed);
    QVERIFY(f.wan("POST", "/phone/incoming").allowed);
    QVERIFY(f.from("10.0.0.5").allowed);
    QVERIFY(f.wan("GET", "/api/browse-pick", {}, kToken, true).allowed);   // agent exemption beats the lockout
    // the lockout ends after 10 minutes and the counter starts over
    clk.advance(Gate::kLockoutMs - 90 * 1000);
    QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));
    QVERIFY(f.wan("GET", "/api/browse", basic(kPassword)).allowed);
    QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));

    // failures older than the window do not count: 19 failures, 10 min later 19 more are not a lockout
    f.gate.reset();
    for (int i = 0; i < Gate::kMaxFailures - 1; ++i) QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    clk.advance(Gate::kLockoutMs + 1);
    for (int i = 0; i < Gate::kMaxFailures - 1; ++i) QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));
    // a success clears the failure count
    QVERIFY(f.wan("GET", "/api/browse", basic(kPassword)).allowed);
    for (int i = 0; i < Gate::kMaxFailures - 1; ++i) QCOMPARE(f.wan("GET", "/api/browse", basic(kWrong)).status, 401);
    QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));
    // failures are keyed by the normalised address: a mapped form shares the counter
    QCOMPARE(f.gate.decide("GET", "/api/browse", ip(std::string("::ffff:") + kWanIp), basic(kWrong), {}, false, false).status, 401);
    QVERIFY(f.gate.isLockedOut(ip(kWanIp)));
    QVERIFY(f.gate.isLockedOut(ip(std::string("::ffff:") + kWanIp)));
    f.gate.reset();
    QVERIFY(!f.gate.isLockedOut(ip(kWanIp)));
}

void TestAnfsCoreGate::gateMiddlewareOverHttp()
{
    TestServer s(makeConfig(fileAccessRules()));
    Gate gate([&s] { return std::shared_ptr<const Config>(s.cfg); });
    s.router.use(gate.middleware());
    auto handled = [](http::Ctx& c) { c.res.sendText(200, "handled"); };
    auto echoBody = [](http::Ctx& c) { c.res.sendText(200, std::to_string(c.body.readAll(1 << 20).size())); };
    s.router.get("/api/browse", handled);
    s.router.get("/api/shares", handled);
    s.router.get("/api/browse-pick", handled, http::kRouteAgentExempt);
    s.router.get("/api/pub/nodes", handled, http::kRouteOpen);
    s.router.get("/api/dropbox/{node}", handled);
    s.router.post("/api/upload/init", echoBody);
    s.router.get("/files/{token}", handled);
    s.router.get("/api/browse/sub", handled);
    s.cfg->wan.gatedPaths.push_back(*GateRule::parse(JVal("GET /api/pub/nodes")));   // open flag beats the list
    s.start();

    const std::string path = "/api/browse?path=C%3A%5C";
    // loopback and LAN (via the trusted proxy) reach the handler
    http::HttpClient::Result r = s.fetch("GET", path);
    QCOMPARE(r.status, 200);
    SEQ(r.body, "handled");
    r = s.fetch("GET", path, {{"X-Forwarded-For", "10.0.0.5"}});
    QCOMPARE(r.status, 200);
    // WAN without credentials: the Node 401
    r = s.fetch("GET", path, {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 401);
    SEQ(r.body, "Authentication required.\n");
    SEQ(r.headers.get("www-authenticate"), "Basic realm=\"AN-FS\", charset=\"UTF-8\"");
    SEQ(r.headers.get("content-type"), "text/plain; charset=utf-8");
    SEQ(r.headers.get("cache-control"), "no-store");
    SEQ(r.headers.get("content-length"), "25");
    QVERIFY(r.headers.get("retry-after").empty());
    // the right password
    r = s.fetch("GET", path, {{"X-Forwarded-For", kWanIp}, {"Authorization", basic(kPassword)}});
    QCOMPARE(r.status, 200);
    SEQ(r.body, "handled");
    r = s.fetch("GET", path, {{"X-Forwarded-For", kWanIp}, {"Authorization", basic(kWrong)}});
    QCOMPARE(r.status, 401);
    // a refused upload: 401 before the body is read, the connection closes
    r = s.fetch("POST", "/api/upload/init", {{"X-Forwarded-For", kWanIp}, {"Content-Type", "application/octet-stream"}},
                std::string(4096, 'x'));
    QCOMPARE(r.status, 401);
    SEQ(r.headers.get("connection"), "close");
    r = s.fetch("POST", "/api/upload/init", {{"X-Forwarded-For", kWanIp}, {"Authorization", basic(kPassword)}},
                std::string(4096, 'x'));
    QCOMPARE(r.status, 200);
    SEQ(r.body, "4096");
    // agent token on an exempt route, refused on another
    r = s.fetch("GET", "/api/browse-pick?path=C%3A%5C", {{"X-Forwarded-For", kWanIp}, {"x-agent-auth", kToken}});
    QCOMPARE(r.status, 200);
    r = s.fetch("GET", "/api/shares", {{"X-Forwarded-For", kWanIp}, {"x-agent-auth", kToken}});
    QCOMPARE(r.status, 401);
    r = s.fetch("GET", std::string("/api/browse-pick?auth=") + kToken, {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 401);                                                // the query form is not honoured
    // open routes and ungated paths pass; the open flag wins over a gate rule
    r = s.fetch("GET", "/api/pub/nodes", {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 200);
    r = s.fetch("GET", "/files/0123456789abcdef", {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 200);
    r = s.fetch("GET", "/api/browse/sub", {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 200);
    // an unknown gated path is refused before the 404
    r = s.fetch("GET", "/api/drives", {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 401);
    r = s.fetch("GET", "/api/drives");
    QCOMPARE(r.status, 404);
    // HEAD on a gated GET route
    r = s.fetch("HEAD", "/api/dropbox/local", {{"X-Forwarded-For", kWanIp}});
    QCOMPARE(r.status, 401);
    QVERIFY(r.body.empty());
    // a WAN peer with a spoofed header stays WAN (the server's own 127.0.0.1 peer is the only trusted hop here)
    r = s.fetch("GET", path, {{"X-Forwarded-For", std::string("10.0.0.5, ") + kWanIp}});
    QCOMPARE(r.status, 401);
    // an unparsable last hop is WAN
    r = s.fetch("GET", path, {{"X-Forwarded-For", "10.0.0.5, nope"}});
    QCOMPARE(r.status, 401);
    // requireTls: 403 text, and the proxy listener's PROXY line marks the request secure
    s.cfg->wan.requireTls = true;
    r = s.fetch("GET", path, {{"X-Forwarded-For", kWanIp}, {"Authorization", basic(kPassword)}});
    QCOMPARE(r.status, 403);
    SEQ(r.body, "HTTPS is required for remote access.\n");
    QVERIFY(r.headers.get("www-authenticate").empty());
    s.cfg->wan.requireTls = false;
    // 20 failures: 429 with Retry-After, the handler never runs; another client is unaffected
    for (int i = 0; i < Gate::kMaxFailures; ++i) {
        r = s.fetch("GET", path, {{"X-Forwarded-For", "198.51.100.7"}, {"Authorization", basic(kWrong)}});
        QCOMPARE(r.status, 401);
    }
    r = s.fetch("GET", path, {{"X-Forwarded-For", "198.51.100.7"}, {"Authorization", basic(kPassword)}});
    QCOMPARE(r.status, 429);
    SEQ(r.body, "Too many failed attempts. Try again later.\n");
    SEQ(r.headers.get("retry-after"), "600");
    QVERIFY(r.headers.get("www-authenticate").empty());
    SEQ(r.headers.get("cache-control"), "no-store");
    r = s.fetch("GET", path, {{"X-Forwarded-For", kWanIp}, {"Authorization", basic(kPassword)}});
    QCOMPARE(r.status, 200);
    QVERIFY(gate.isLockedOut(ip("198.51.100.7")));
    QVERIFY(!gate.isLockedOut(ip(kWanIp)));

    // without resolveClient the server marks everyone WAN: the gate fails closed even for loopback
    TestServer bare(makeConfig(fileAccessRules()));
    Gate gate2([&bare] { return std::shared_ptr<const Config>(bare.cfg); });
    http::ServerOptions none;
    http::Server plain(bare.router, [&bare] { return std::shared_ptr<const Config>(bare.cfg); }, nullptr, none);
    bare.router.use(gate2.middleware());
    bare.router.get("/api/browse", handled);
    plain.start(0, false, "127.0.0.1");
    const std::string base = "http://127.0.0.1:" + std::to_string(plain.port());
    r = bare.client.fetch("GET", base + "/api/browse");
    QCOMPARE(r.status, 401);
    r = bare.client.fetch("GET", base + "/phone/incoming");
    QCOMPARE(r.status, 404);
    http::Headers h;
    h.add("Authorization", basic(kPassword));
    r = bare.client.fetch("GET", base + "/api/browse", h);
    QCOMPARE(r.status, 200);
    plain.stop();
}

// ---- CORS ------------------------------------------------------------------------------------------------------------

void TestAnfsCoreGate::corsHeadersAndOrigins()
{
    using anfs::cors::HeaderList;
    auto value = [](const HeaderList& h, const char* name) -> std::string {
        for (const auto& [k, v] : h)
            if (_stricmp(k.c_str(), name) == 0) return v;
        return "<absent>";
    };
    auto count = [](const HeaderList& h, const char* name) {
        int n = 0;
        for (const auto& [k, v] : h) n += _stricmp(k.c_str(), name) == 0;
        return n;
    };

    // peer: reflect any Origin with credentials, "*" without one
    HeaderList p = cors::peerHeaders("https://evil.example");
    SEQ(value(p, "Access-Control-Allow-Origin"), "https://evil.example");
    SEQ(value(p, "Access-Control-Allow-Credentials"), "true");
    SEQ(value(p, "Access-Control-Allow-Methods"), "GET, POST, PUT, DELETE, OPTIONS");
    SEQ(value(p, "Access-Control-Allow-Headers"), "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index");
    QCOMPARE(count(p, "Vary"), 0);
    QCOMPARE(p.size(), size_t(4));
    SEQ(p[0].first, "Access-Control-Allow-Origin");          // order as agent.js sets them
    SEQ(p[1].first, "Access-Control-Allow-Credentials");
    p = cors::peerHeaders("");
    SEQ(value(p, "Access-Control-Allow-Origin"), "*");
    QCOMPARE(count(p, "Access-Control-Allow-Credentials"), 0);
    QCOMPARE(p.size(), size_t(3));

    // server: the wan-gate.js allow-list
    for (const char* o : {"https://wyat.getstarnews.com", "http://wyat.getstarnews.com", "https://files.wyat.getstarnews.com",
                          "https://a.b.wyat.getstarnews.com:8443", "http://localhost:3000", "http://127.0.0.1:80",
                          "http://[::1]:8080", "http://192.168.1.177", "http://192.168.1.20:8899", "http://172.16.4.4",
                          "http://100.64.0.5:3000", "https://10.0.0.5", "HTTPS://WYAT.GETSTARNEWS.COM",
                          "https://wyat.getstarnews.com.", "http://app.localhost"})
        QVERIFY2(cors::serverOriginAllowed(o, {}), o);
    for (const char* o : {"https://evil.example", "https://getstarnews.com", "https://www.getstarnews.com",
                          "https://wyat.getstarnews.com.evil.example", "https://evilwyat.getstarnews.com",
                          "https://xwyat.getstarnews.com", "http://203.0.113.7", "http://172.32.0.1", "http://100.128.0.1",
                          "http://localhost.evil.example", "null", "file://", "ftp://wyat.getstarnews.com", "",
                          "wyat.getstarnews.com", "http://10.0.0.5.evil.example", "http://[2001:db8::5]"})
        QVERIFY2(!cors::serverOriginAllowed(o, {}), o);
    // config.corsAllow entries (C# EntryMatches forms)
    const std::vector<std::string> extra{"https://app.example:8443", "example.test", "*.cdn.example", "portal.example:8080", "*", " Trailing.Example/ "};
    for (const char* o : {"https://app.example:8443", "http://example.test", "https://example.test:444", "https://x.cdn.example",
                          "https://a.b.cdn.example", "http://portal.example:8080", "https://trailing.example"})
        QVERIFY2(cors::serverOriginAllowed(o, extra), o);
    for (const char* o : {"http://app.example:8443", "https://app.example", "https://cdn.example", "https://notcdn.example",
                          "http://portal.example", "http://portal.example:8081", "https://evil.example"})
        QVERIFY2(!cors::serverOriginAllowed(o, extra), o);
    // C# Uri.Authority omits a default port, so a "host:port" entry never matches an origin on the default port
    QVERIFY(!cors::serverOriginAllowed("http://portal.example", {"portal.example:80"}));
    QVERIFY(!cors::serverOriginAllowed("https://portal.example", {"portal.example:443"}));
    QVERIFY(!cors::serverOriginAllowed("http://portal.example:80", {"portal.example:80"}));   // ":80" is dropped
    QVERIFY(cors::serverOriginAllowed("http://portal.example:80", {"portal.example"}));
    QVERIFY(cors::serverOriginAllowed("http://[::1]:5000", {"[::1]:5000"}));

    HeaderList s = cors::serverHeaders("https://wyat.getstarnews.com", {});
    SEQ(value(s, "Access-Control-Allow-Origin"), "https://wyat.getstarnews.com");
    SEQ(value(s, "Access-Control-Allow-Credentials"), "true");
    SEQ(value(s, "Vary"), "Origin");
    SEQ(value(s, "Access-Control-Allow-Methods"), "GET, POST, PUT, DELETE, OPTIONS");
    SEQ(value(s, "Access-Control-Allow-Headers"), "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index");
    QCOMPARE(s.size(), size_t(5));
    s = cors::serverHeaders("https://evil.example", {});
    SEQ(value(s, "Access-Control-Allow-Origin"), "*");
    QCOMPARE(count(s, "Access-Control-Allow-Credentials"), 0);
    SEQ(value(s, "Vary"), "Origin");
    QCOMPARE(s.size(), size_t(4));
    s = cors::serverHeaders("", {});
    SEQ(value(s, "Access-Control-Allow-Origin"), "*");
    QCOMPARE(count(s, "Vary"), 0);
    QCOMPARE(s.size(), size_t(3));
    s = cors::serverHeaders("https://evil.example", {"evil.example"});
    SEQ(value(s, "Access-Control-Allow-Origin"), "https://evil.example");
    SEQ(value(s, "Access-Control-Allow-Credentials"), "true");
}

void TestAnfsCoreGate::corsMiddlewareOverHttp()
{
    // peer flavour
    {
        TestServer s(makeConfig({}));
        s.router.useFirst(cors::peerMiddleware());
        s.router.get("/api/setup", [](http::Ctx& c) { c.res.sendText(200, "ok"); });
        s.start();
        http::HttpClient::Result r = s.fetch("GET", "/api/setup", {{"Origin", "https://evil.example"}});
        QCOMPARE(r.status, 200);
        SEQ(r.headers.get("access-control-allow-origin"), "https://evil.example");
        SEQ(r.headers.get("access-control-allow-credentials"), "true");
        SEQ(r.headers.get("access-control-allow-methods"), "GET, POST, PUT, DELETE, OPTIONS");
        SEQ(r.headers.get("access-control-allow-headers"), "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index");
        QVERIFY(r.headers.get("vary").empty());
        r = s.fetch("GET", "/api/setup");
        SEQ(r.headers.get("access-control-allow-origin"), "*");
        QVERIFY(r.headers.get("access-control-allow-credentials").empty());
        r = s.fetch("OPTIONS", "/api/setup", {{"Origin", "https://evil.example"}, {"Access-Control-Request-Method", "POST"}});
        QCOMPARE(r.status, 200);
        SEQ(r.body, "OK");
        SEQ(r.headers.get("content-type"), "text/plain; charset=utf-8");
        SEQ(r.headers.get("access-control-allow-origin"), "https://evil.example");
        r = s.fetch("OPTIONS", "/no/such/route");
        QCOMPARE(r.status, 200);                                   // before routing, like app.use
        SEQ(r.body, "OK");
    }
    // server flavour, in front of the gate
    {
        TestServer s(makeConfig(fileAccessRules()));
        Gate gate([&s] { return std::shared_ptr<const Config>(s.cfg); });
        s.router.use(gate.middleware());
        s.router.useFirst(cors::serverMiddleware());
        s.router.get("/api/browse", [](http::Ctx& c) { c.res.sendText(200, "ok"); });
        s.start();
        for (const char* o : {"https://evil.example", "https://wyat.getstarnews.com.evil.example", "null"}) {
            http::HttpClient::Result r = s.fetch("GET", "/api/browse", {{"Origin", o}});
            QCOMPARE(r.status, 200);
            SEQ(r.headers.get("access-control-allow-origin"), "*");
            QVERIFY2(r.headers.get("access-control-allow-credentials").empty(), o);
            SEQ(r.headers.get("vary"), "Origin");
        }
        for (const char* o : {"https://wyat.getstarnews.com", "https://files.wyat.getstarnews.com", "http://localhost:5173",
                              "http://192.168.1.177", "http://100.64.0.5:3000"}) {
            http::HttpClient::Result r = s.fetch("GET", "/api/browse", {{"Origin", o}});
            QCOMPARE(r.status, 200);
            SEQ(r.headers.get("access-control-allow-origin"), o);
            SEQ(r.headers.get("access-control-allow-credentials"), "true");
            SEQ(r.headers.get("vary"), "Origin");
        }
        http::HttpClient::Result r = s.fetch("GET", "/api/browse");
        QCOMPARE(r.status, 200);
        SEQ(r.headers.get("access-control-allow-origin"), "*");
        QVERIFY(r.headers.get("access-control-allow-credentials").empty());
        QVERIFY(r.headers.get("vary").empty());
        SEQ(r.headers.get("access-control-allow-methods"), "GET, POST, PUT, DELETE, OPTIONS");
        SEQ(r.headers.get("access-control-allow-headers"), "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index");
        // same-origin calls through the WAN gate keep working; the 401 carries no credentials for a foreign Origin
        r = s.fetch("GET", "/api/browse", {{"X-Forwarded-For", kWanIp}, {"Origin", "https://wyat.getstarnews.com"}, {"Authorization", basic(kPassword)}});
        QCOMPARE(r.status, 200);
        SEQ(r.headers.get("access-control-allow-credentials"), "true");
        r = s.fetch("GET", "/api/browse", {{"X-Forwarded-For", kWanIp}, {"Origin", "https://evil.example"}});
        QCOMPARE(r.status, 401);
        QVERIFY(r.headers.get("access-control-allow-credentials").empty());
        SEQ(r.headers.get("access-control-allow-origin"), "*");
        r = s.fetch("GET", "/api/browse", {{"X-Forwarded-For", kWanIp}, {"Origin", "https://wyat.getstarnews.com"}});
        QCOMPARE(r.status, 401);
        SEQ(r.headers.get("access-control-allow-credentials"), "true");   // the 401 still carries CORS for an allowed Origin
        // OPTIONS preflight: 204 with no body (D1), before the gate, for any origin
        r = s.fetch("OPTIONS", "/api/browse", {{"X-Forwarded-For", kWanIp}, {"Origin", "https://evil.example"}, {"Access-Control-Request-Method", "GET"}});
        QCOMPARE(r.status, 204);
        QVERIFY(r.body.empty());
        QVERIFY(r.headers.get("access-control-allow-credentials").empty());
        QVERIFY(r.headers.get("content-type").empty());
        r = s.fetch("OPTIONS", "/api/browse", {{"X-Forwarded-For", kWanIp}, {"Origin", "https://wyat.getstarnews.com"}});
        QCOMPARE(r.status, 204);
        SEQ(r.headers.get("access-control-allow-credentials"), "true");
        // config.corsAllow is read from the request's snapshot
        s.cfg->corsAllow = {"evil.example"};
        r = s.fetch("GET", "/api/browse", {{"Origin", "https://evil.example"}});
        SEQ(r.headers.get("access-control-allow-origin"), "https://evil.example");
        SEQ(r.headers.get("access-control-allow-credentials"), "true");
    }
}

QTEST_GUILESS_MAIN(TestAnfsCoreGate)
#include "test_anfs_core_gate.moc"
