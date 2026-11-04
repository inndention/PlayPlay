// test_anfs_core_runtime - ANFS-F7: the module registry (core/module.cpp), the ModeManager, the status writer
// (core/status.cpp) and the anfs.exe CLI (src/main.cpp).
//
// - Registry: (order, seq) sorting, mode lists ("common" runs in peer and server, never in off), create() per mode.
// - ModeManager in-process with a private registry of recording modules: off -> peer -> server -> off by rewriting
//   anfs.json (ConfigStore::load -> listener -> apply); modules start in order and stop in reverse, the server answers
//   on the new port and the old port closes; a port change rebinds; a bind failure (port held by a raw socket) rolls
//   back to the previous mode and port with lastError, appliedRev stays, and tick() retries after retryMs; a module
//   whose start throws is recorded as {"error": ...} without stopping the others; the stop budget.
// - Test-mode guards: ports 8080/8899/443 and a non-loopback bridge are refused (apply keeps the running state);
//   allowRemoteBridge and test:false pass.
// - status.json: field order, values per mode, the 2 s timer on a ManualClock, extras, a failed write logged once.
// - anfs.exe as a child: a second `run` on the same data folder exits 3 (and the mutex name follows the C# formula);
//   `run` in mode off writes status.json and serves 127.0.0.1:<statusPort>/status; `config set` is applied live
//   (appliedRev follows rev; the peer port opens and closes); `config set` refuses a live port in test mode; eight
//   parallel `config set` bump rev by exactly eight; hash-password prints a verifying record and never echoes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "core/clock.h"
#include "core/config.h"
#include "core/gate.h"
#include "core/http_client.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "core/module.h"
#include "core/status.h"
#include "core/svc.h"
#include "core/version.h"

#include "anfs_testkit.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtTest/QtTest>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace anfs;

#define SEQ(a, b) QCOMPARE(QString::fromUtf8(std::string(a).c_str()), QString::fromUtf8(std::string(b).c_str()))

namespace {

struct Wsa {
    Wsa() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { WSACleanup(); }
};

// A free loopback port (bound and released; the usual small race is fine for a test).
int freePort()
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        int len = sizeof a;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
    }
    closesocket(s);
    return port;
}

// Holds a loopback port so a bind on it fails with 10048.
struct PortHolder {
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    explicit PortHolder(int p)
    {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<u_short>(p));
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(s, 1) == 0) port = p;
    }
    ~PortHolder() { release(); }
    void release()
    {
        if (s != INVALID_SOCKET) closesocket(s);
        s = INVALID_SOCKET;
    }
};

// Recording modules: every start/stop goes to one shared event list, in call order.
struct Events {
    std::mutex mu;
    std::vector<std::string> list;
    void add(const std::string& e) { std::lock_guard<std::mutex> lk(mu); list.push_back(e); }
    std::vector<std::string> take() { std::lock_guard<std::mutex> lk(mu); std::vector<std::string> out; out.swap(list); return out; }
};
Events g_events;

class RecModule : public Module {
public:
    RecModule(std::string name, bool throwOnStart = false, int stopDelayMs = 0)
        : name_(std::move(name)), throwOnStart_(throwOnStart), stopDelayMs_(stopDelayMs) {}
    const char* name() const override { return name_.c_str(); }
    void start(Context& ctx) override
    {
        g_events.add("start " + name_);
        if (throwOnStart_) throw std::runtime_error(name_ + " refuses to start");
        ctx_ = &ctx;
        port_ = ctx.port;
        mode_ = ctx.mode;
        generation_ = ctx.generation;
        ctx.http.get("/api/" + name_, [this](http::Ctx& c) { c.json(200, JVal::object({{"module", name_}, {"hits", ++hits_}})); }, 0, name_);
        started_ = true;
    }
    void stop() override
    {
        g_events.add("stop " + name_);
        if (stopDelayMs_ > 0) QThread::msleep(stopDelayMs_);
        if (ctx_) ctx_->http.removeOwner(name_);
        ctx_ = nullptr;
        started_ = false;
    }
    JVal status() const override
    {
        return JVal::object({{"running", started_}, {"port", port_}, {"mode", mode_}, {"generation", generation_}, {"hits", hits_}});
    }

private:
    std::string name_;
    bool throwOnStart_;
    int stopDelayMs_;
    Context* ctx_ = nullptr;
    bool started_ = false;
    int port_ = 0;
    std::string mode_;
    int generation_ = 0;
    int hits_ = 0;
};

// One in-process runtime: temp data folder, ConfigStore + listener -> ModeManager, StatusWriter (file in the folder).
struct Runtime {
    QTemporaryDir tmp;
    std::string dataDir;
    Log log;
    ConfigStore store;
    ModuleRegistry registry;
    std::unique_ptr<ModeManager> mm;
    StatusWriter status;
    int64_t started = 0;
    int sub = -1;
    http::HttpClient client;

    Runtime()
        : dataDir(QDir::toNativeSeparators(tmp.path()).toStdString()), log(), store(dataDir + "\\anfs.json"),
          status(dataDir + "\\status.json", [this] { return mm ? buildStatus(*mm, store, log, started) : JVal::object(); }, &log)
    {
        log.setConsole(false);
        started = anfs::nowMs();
    }
    ~Runtime()
    {
        if (sub >= 0) store.unsubscribe(sub);
        if (mm) { mm->shutdown(); mm.reset(); }   // while `status` (destroyed first otherwise) is still alive
        Svc::clear();
    }
    void init()
    {
        mm = std::make_unique<ModeManager>(store, log, registry, dataDir, status);
        sub = store.subscribe([this](std::shared_ptr<const Config> c) { mm->apply(c); });
    }
    // Rewrites anfs.json like an external writer and lets the store pick it up (the watcher's job in anfs run).
    bool write(const Config& c)
    {
        std::string err;
        if (!writeFileAtomic(store.path(), c.toJsonText(), &err)) return false;
        return store.load();
    }
    http::HttpClient::Result fetch(int port, const std::string& path)
    {
        return client.fetch("GET", "http://127.0.0.1:" + std::to_string(port) + path, {}, {}, 3000);
    }
    static Config cfg(const char* mode, int port, int64_t rev)
    {
        Config c;
        c.mode = mode;
        c.port = port;
        c.bindAll = false;                        // loopback only: no firewall prompt on this box
        c.test = true;
        c.rev = rev;
        c.nodeId = "node-runtime-test";
        c.name = "runtime";
        return c;
    }
};

std::vector<std::string> keysOf(const JVal& o)
{
    std::vector<std::string> out;
    for (const auto& kv : o.obj()) out.push_back(kv.first);
    return out;
}

QString joinKeys(const std::vector<std::string>& v)
{
    QStringList l;
    for (const std::string& s : v) l << QString::fromStdString(s);
    return l.join(",");
}

std::string readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

// ---- anfs.exe as a child process -------------------------------------------------------------------------------------

struct Run {
    int exit = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    std::string out;
    std::string err;
};

Run runAnfs(const QStringList& args, int timeoutMs = 30000, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProgram(QString::fromUtf8(ANFS_EXE_PATH));
    p.setArguments(args);
    p.setProcessEnvironment(env);
    p.start();
    Run r;
    if (!p.waitForStarted(10000)) {
        r.err = "did not start";
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(5000);
        r.err = "timeout";
        return r;
    }
    r.exit = p.exitCode();
    r.status = p.exitStatus();
    // The CRT text mode writes CRLF (C# Console.WriteLine does too); compare LF-only text.
    auto lf = [](QByteArray b) { b.replace('\r', ""); return b.toStdString(); };
    r.out = lf(p.readAllStandardOutput());
    r.err = lf(p.readAllStandardError());
    return r;
}

struct Child {
    QProcess p;
    bool start(const QString& dataDir)
    {
        p.setProgram(QString::fromUtf8(ANFS_EXE_PATH));
        p.setArguments({"run", "--data", dataDir});
        p.start();
        return p.waitForStarted(10000);
    }
    ~Child()
    {
        if (p.state() != QProcess::NotRunning) {
            p.kill();                             // StarRoot kills hard too; nothing relies on a graceful exit
            p.waitForFinished(10000);
        }
    }
};

template <class Pred> bool waitReal(Pred pred, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return pred();
        QThread::msleep(25);
    }
    return true;
}

JVal statusFile(const std::string& dataDir)
{
    JVal v;
    const std::string text = readAll(QString::fromStdString(dataDir + "\\status.json"));
    if (text.empty() || !json::tryParse(text, v)) return JVal();
    return v;
}

} // namespace

class TestAnfsCoreRuntime : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void registryOrderAndModes();
    void modeSwitchInProcess();
    void portChangeAndBindFailureRollback();
    void moduleStartErrorAndStopOrder();
    void testModeGuards();
    void statusWriterAndFields();
    void singleInstanceMutex();
    void cliRunStatusEndpointAndConfigSet();
    void cliConfigSetBumpsRevAtomically();
    void cliHashPasswordAndUsage();

    // ANFS-F8: the shared testkit (anfs_testkit.h), the anfs_sim skeleton and the harness skeleton.
    void testkitServerAndRawClient();
    void testkitFakePeerAndMemProbe();
    void testkitModuleHost();
    void simGuardsAndBridge();
    void harnessGuardsAndSmoke();

private:
    std::unique_ptr<Wsa> wsa_;
};

void TestAnfsCoreRuntime::initTestCase()
{
    wsa_ = std::make_unique<Wsa>();
    QVERIFY2(QFileInfo::exists(QString::fromUtf8(ANFS_EXE_PATH)), "anfs.exe must be built (ANFS_EXE_PATH)");
}

void TestAnfsCoreRuntime::cleanupTestCase() { wsa_.reset(); }

// ---- registry --------------------------------------------------------------------------------------------------------

void TestAnfsCoreRuntime::registryOrderAndModes()
{
    ModuleRegistry r;
    r.add("server", 20, [] { return std::unique_ptr<Module>(new RecModule("srv-b")); }, "SrvB");
    r.add("common", 100, [] { return std::unique_ptr<Module>(new RecModule("common-100")); }, "Common100");
    r.add("peer, server", 10, [] { return std::unique_ptr<Module>(new RecModule("both-10")); }, "Both10");
    r.add("peer", 10, [] { return std::unique_ptr<Module>(new RecModule("peer-10b")); }, "Peer10b");
    r.add("Common", 5, [] { return std::unique_ptr<Module>(new RecModule("common-5")); }, "Common5");
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument, r.add("relay", 1, [] { return std::unique_ptr<Module>(); }, "Bad"));

    const std::vector<ModuleInfo> l = r.list();
    QCOMPARE(l.size(), size_t(5));
    SEQ(l[0].className, "Common5");               // order 5
    SEQ(l[1].className, "Both10");                // order 10, seq 2
    SEQ(l[2].className, "Peer10b");               // order 10, seq 3
    SEQ(l[3].className, "SrvB");                  // order 20
    SEQ(l[4].className, "Common100");
    QVERIFY(l[1].runsIn("peer") && l[1].runsIn("server") && !l[1].runsIn("off"));
    QVERIFY(l[0].runsIn("PEER") && !l[0].runsIn("off") && !l[0].runsIn(""));
    QVERIFY(!l[3].runsIn("peer") && l[3].runsIn("server"));

    auto names = [](const std::vector<std::unique_ptr<Module>>& ms) {
        QStringList out;
        for (const auto& m : ms) out << m->name();
        return out.join(",");
    };
    QCOMPARE(names(r.create("peer")), QString("common-5,both-10,peer-10b,common-100"));
    QCOMPARE(names(r.create("server")), QString("common-5,both-10,srv-b,common-100"));
    QCOMPARE(names(r.create("off")), QString(""));

    // The process-wide registry exists and this test binary registered nothing in it (no ANFS_MODULE lines yet).
    QVERIFY(&ModuleRegistry::global() == &ModuleRegistry::global());
    QVERIFY(ModuleRegistry::global().create("server").empty());
}

// ---- ModeManager -------------------------------------------------------------------------------------------------------

void TestAnfsCoreRuntime::modeSwitchInProcess()
{
    Runtime rt;
    rt.registry.add("common", 10, [] { return std::unique_ptr<Module>(new RecModule("alpha")); }, "Alpha");
    rt.registry.add("peer", 20, [] { return std::unique_ptr<Module>(new RecModule("peer-only")); }, "PeerOnly");
    rt.registry.add("server", 20, [] { return std::unique_ptr<Module>(new RecModule("server-only")); }, "ServerOnly");
    rt.registry.add("common", 30, [] { return std::unique_ptr<Module>(new RecModule("omega")); }, "Omega");
    rt.init();
    g_events.take();

    // mode off: nothing runs, nothing binds, the status file is written by apply()
    QVERIFY(rt.write(Runtime::cfg("off", 0, 1)));
    QVERIFY(g_events.take().empty());
    SEQ(rt.mm->mode(), "off");
    QCOMPARE(rt.mm->appliedRev(), int64_t(1));
    QCOMPARE(rt.mm->generation(), 0);
    QVERIFY(rt.mm->server() == nullptr);
    QVERIFY(rt.mm->moduleNames().empty());
    QVERIFY(QFileInfo::exists(QString::fromStdString(rt.dataDir + "\\status.json")));

    // off -> peer: modules in order, the server answers on the port, ctx carries port/mode/generation
    const int p1 = freePort();
    QVERIFY(p1 > 0);
    QVERIFY(rt.write(Runtime::cfg("peer", p1, 2)));
    QCOMPARE(joinKeys(g_events.take()), QString("start alpha,start peer-only,start omega"));
    SEQ(rt.mm->mode(), "peer");
    QCOMPARE(rt.mm->port(), p1);
    QCOMPARE(rt.mm->appliedRev(), int64_t(2));
    QCOMPARE(rt.mm->generation(), 1);
    QVERIFY(!rt.mm->failed());
    QVERIFY(rt.mm->lastError().empty());
    QCOMPARE(joinKeys(rt.mm->moduleNames()), QString("alpha,peer-only,omega"));
    http::HttpClient::Result r = rt.fetch(p1, "/api/peer-only");
    QCOMPARE(r.status, 200);
    SEQ(r.body, "{\"module\":\"peer-only\",\"hits\":1}");
    r = rt.fetch(p1, "/api/server-only");
    QCOMPARE(r.status, 404);
    SEQ(r.body, "{\"error\":\"Not found\"}");
    JVal ms = rt.mm->moduleStatuses();
    QCOMPARE(joinKeys(keysOf(ms)), QString("alpha,peer-only,omega"));
    QCOMPARE(ms.get("peer-only").getInt("port"), int64_t(p1));
    SEQ(ms.get("peer-only").getStr("mode"), "peer");
    QCOMPARE(ms.get("peer-only").getInt("generation"), int64_t(1));
    QCOMPARE(ms.get("peer-only").getInt("hits"), int64_t(1));
    QVERIFY(QFileInfo::exists(QString::fromStdString(rt.dataDir + "\\anfs.db")));
    QVERIFY(QFileInfo::exists(QString::fromStdString(rt.dataDir + "\\temp_uploads_agent")));

    // a plain edit (same mode/port/bind) restarts nothing and moves appliedRev
    Config edit = Runtime::cfg("peer", p1, 3);
    edit.name = "renamed";
    QVERIFY(rt.write(edit));
    QVERIFY(g_events.take().empty());
    QCOMPARE(rt.mm->appliedRev(), int64_t(3));
    QCOMPARE(rt.mm->generation(), 1);
    SEQ(rt.store.current()->name, "renamed");

    // peer -> server on another port: old modules stop in reverse, new ones start in order, p1 closes
    const int p2 = freePort();
    QVERIFY(p2 > 0 && p2 != p1);
    QVERIFY(rt.write(Runtime::cfg("server", p2, 4)));
    QCOMPARE(joinKeys(g_events.take()),
             QString("stop omega,stop peer-only,stop alpha,start alpha,start server-only,start omega"));
    SEQ(rt.mm->mode(), "server");
    QCOMPARE(rt.mm->port(), p2);
    QCOMPARE(rt.mm->generation(), 2);
    QCOMPARE(rt.mm->appliedRev(), int64_t(4));
    r = rt.fetch(p2, "/api/server-only");
    QCOMPARE(r.status, 200);
    r = rt.fetch(p1, "/api/alpha");
    QCOMPARE(r.status, 0);                        // nothing listens on p1 any more
    QVERIFY(QFileInfo::exists(QString::fromStdString(rt.dataDir + "\\temp_uploads")));
    // server mode installed the CORS + gate middleware (loopback passes; an allowed Origin is reflected)
    {
        http::Headers h;
        h.add("Origin", "http://localhost:3000");
        http::HttpClient::Result c = rt.client.fetch("GET", "http://127.0.0.1:" + std::to_string(p2) + "/api/alpha", h, {}, 3000);
        QCOMPARE(c.status, 200);
        SEQ(c.headers.get("access-control-allow-origin"), "http://localhost:3000");
        SEQ(c.headers.get("access-control-allow-credentials"), "true");
    }

    // server -> off: everything stops in reverse, the port closes, generation stays
    QVERIFY(rt.write(Runtime::cfg("off", p2, 5)));
    QCOMPARE(joinKeys(g_events.take()), QString("stop omega,stop server-only,stop alpha"));
    SEQ(rt.mm->mode(), "off");
    QCOMPARE(rt.mm->appliedRev(), int64_t(5));
    QVERIFY(rt.mm->server() == nullptr);
    QVERIFY(rt.mm->moduleNames().empty());
    r = rt.fetch(p2, "/api/alpha");
    QCOMPARE(r.status, 0);

    // shutdown is idempotent
    rt.mm->shutdown();
    rt.mm->shutdown();
    QVERIFY(g_events.take().empty());
}

void TestAnfsCoreRuntime::portChangeAndBindFailureRollback()
{
    Runtime rt;
    rt.registry.add("common", 10, [] { return std::unique_ptr<Module>(new RecModule("alpha")); }, "Alpha");
    rt.init();
    g_events.take();

    const int p1 = freePort();
    QVERIFY(rt.write(Runtime::cfg("peer", p1, 1)));
    QCOMPARE(rt.fetch(p1, "/api/alpha").status, 200);
    g_events.take();

    // a port change rebinds (same mode): stop, start, the new port answers, the old one is closed
    const int p2 = freePort();
    QVERIFY(rt.write(Runtime::cfg("peer", p2, 2)));
    QCOMPARE(joinKeys(g_events.take()), QString("stop alpha,start alpha"));
    QCOMPARE(rt.mm->port(), p2);
    QCOMPARE(rt.fetch(p2, "/api/alpha").status, 200);
    QCOMPARE(rt.fetch(p1, "/api/alpha").status, 0);
    QCOMPARE(rt.mm->appliedRev(), int64_t(2));

    // a bind failure rolls back to the previous mode and port with lastError; appliedRev stays
    const int p3 = freePort();
    PortHolder held(p3);
    QVERIFY2(held.port == p3, "could not hold the port");
    QVERIFY(rt.write(Runtime::cfg("server", p3, 3)));
    QCOMPARE(joinKeys(g_events.take()), QString("stop alpha,start alpha"));   // torn down, then rebuilt as peer
    QVERIFY(rt.mm->failed());
    QVERIFY2(rt.mm->lastError().find("http start failed") != std::string::npos, rt.mm->lastError().c_str());
    QVERIFY2(rt.mm->lastError().find(std::to_string(p3)) != std::string::npos, rt.mm->lastError().c_str());
    QVERIFY(rt.mm->lastErrorAtMs() > 0);
    SEQ(rt.mm->mode(), "peer");
    QCOMPARE(rt.mm->port(), p2);
    QCOMPARE(rt.mm->appliedRev(), int64_t(2));
    QCOMPARE(rt.fetch(p2, "/api/alpha").status, 200);
    JVal st = buildStatus(*rt.mm, rt.store, rt.log, rt.started);
    SEQ(st.getStr("mode"), "peer");
    QCOMPARE(st.getInt("rev"), int64_t(3));
    QCOMPARE(st.getInt("appliedRev"), int64_t(2));
    QVERIFY(st.get("lastError").isString());
    QVERIFY(st.get("lastErrorTs").isString());

    // tick() before retryMs does nothing; after it (port still held) the retry fails and rolls back again
    rt.mm->setRetryMs(200);
    rt.mm->tick();
    QVERIFY(g_events.take().empty());
    QThread::msleep(250);
    rt.mm->tick();
    QCOMPARE(joinKeys(g_events.take()), QString("stop alpha,start alpha"));
    QVERIFY(rt.mm->failed());
    SEQ(rt.mm->mode(), "peer");

    // the port is released: the next retry applies the wanted config
    held.release();
    QThread::msleep(250);
    rt.mm->tick();
    QCOMPARE(joinKeys(g_events.take()), QString("stop alpha,start alpha"));
    QVERIFY(!rt.mm->failed());
    QVERIFY(rt.mm->lastError().empty());
    SEQ(rt.mm->mode(), "server");
    QCOMPARE(rt.mm->port(), p3);
    QCOMPARE(rt.mm->appliedRev(), int64_t(3));
    QCOMPARE(rt.fetch(p3, "/api/alpha").status, 200);
    QCOMPARE(rt.fetch(p2, "/api/alpha").status, 0);

    // a bind failure from mode off (nothing to roll back to but off) leaves off running and failed set
    QVERIFY(rt.write(Runtime::cfg("off", 0, 4)));
    g_events.take();
    PortHolder held2(p1);
    QVERIFY(rt.write(Runtime::cfg("peer", p1, 5)));
    QCOMPARE(joinKeys(g_events.take()), QString(""));
    QVERIFY(rt.mm->failed());
    SEQ(rt.mm->mode(), "off");
    QCOMPARE(rt.mm->appliedRev(), int64_t(4));
    QVERIFY(rt.mm->server() == nullptr);
}

void TestAnfsCoreRuntime::moduleStartErrorAndStopOrder()
{
    Runtime rt;
    rt.registry.add("common", 10, [] { return std::unique_ptr<Module>(new RecModule("first")); }, "First");
    rt.registry.add("common", 20, [] { return std::unique_ptr<Module>(new RecModule("broken", true)); }, "Broken");
    rt.registry.add("common", 30, [] { return std::unique_ptr<Module>(new RecModule("slow", false, 150)); }, "Slow");
    rt.registry.add("common", 40, [] { return std::unique_ptr<Module>(new RecModule("last")); }, "Last");
    rt.init();
    g_events.take();

    const int p = freePort();
    QVERIFY(rt.write(Runtime::cfg("peer", p, 1)));
    // broken's stop() is called once after its failed start; the others continue
    QCOMPARE(joinKeys(g_events.take()), QString("start first,start broken,stop broken,start slow,start last"));
    QCOMPARE(joinKeys(rt.mm->moduleNames()), QString("first,slow,last"));
    JVal ms = rt.mm->moduleStatuses();
    QCOMPARE(joinKeys(keysOf(ms)), QString("first,broken,slow,last"));
    SEQ(json::stringify(ms.get("broken")), "{\"error\":\"broken refuses to start\"}");
    QVERIFY(ms.get("first").getBool("running"));
    QVERIFY2(rt.mm->lastError().find("module broken failed to start: broken refuses to start") != std::string::npos,
             rt.mm->lastError().c_str());
    QVERIFY(!rt.mm->failed());                    // a module error is not an apply failure
    QCOMPARE(rt.mm->appliedRev(), int64_t(1));
    QCOMPARE(rt.fetch(p, "/api/last").status, 200);

    // shutdown: reverse order, the broken one is skipped, and the whole stop stays well inside the 2 s budget
    QElapsedTimer t;
    t.start();
    rt.mm->shutdown();
    const qint64 took = t.elapsed();
    QCOMPARE(joinKeys(g_events.take()), QString("stop last,stop slow,stop first"));
    QVERIFY2(took < 2000, qPrintable(QString::number(took)));
    QVERIFY(took >= 150);
    QCOMPARE(rt.fetch(p, "/api/last").status, 0);
    SEQ(rt.mm->mode(), "off");
    QVERIFY(rt.mm->moduleStatuses().obj().empty());
}

void TestAnfsCoreRuntime::testModeGuards()
{
    auto with = [](const char* mode, int port, bool test, std::vector<std::string> bridges = {}) {
        Config c;
        c.mode = mode;
        c.port = port;
        c.test = test;
        c.bridgeUrls = std::move(bridges);
        return c;
    };
    SEQ(ModeManager::testModeRefusal(with("server", 8080, false)), "");
    SEQ(ModeManager::testModeRefusal(with("off", 8080, true)), "");
    SEQ(ModeManager::testModeRefusal(with("server", 0, true)), "");          // 18080
    SEQ(ModeManager::testModeRefusal(with("peer", 0, true)), "");            // 18899
    SEQ(ModeManager::testModeRefusal(with("server", 8080, true)), "test mode refuses port 8080 (a live AN-FS port)");
    SEQ(ModeManager::testModeRefusal(with("peer", 8899, true)), "test mode refuses port 8899 (a live AN-FS port)");
    SEQ(ModeManager::testModeRefusal(with("server", 443, true)), "test mode refuses port 443 (a live AN-FS port)");
    Config tls = with("server", 18080, true);
    tls.tlsPort = 443;
    SEQ(ModeManager::testModeRefusal(tls), "test mode refuses TLS port 443 (a live AN-FS port)");
    tls.tlsPort = 18443;
    SEQ(ModeManager::testModeRefusal(tls), "");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, true, {"http://127.0.0.1:18080"})), "");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, true, {"http://localhost:18080", "https://[::1]:18443/"})), "");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, true, {"http://127.0.0.1:18080", "http://192.168.1.177:8080"})),
        "test mode refuses the non-loopback bridge http://192.168.1.177:8080 (set allowRemoteBridge:true to allow it)");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, true, {"https://example.invalid"})),
        "test mode refuses the non-loopback bridge https://example.invalid (set allowRemoteBridge:true to allow it)");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, true, {"not a url"})),
        "test mode refuses the bridge URL 'not a url' (not a valid http(s) URL)");
    Config allowed = with("peer", 18899, true, {"http://192.168.1.177:8080"});
    allowed.raw.set("allowRemoteBridge", true);
    SEQ(ModeManager::testModeRefusal(allowed), "");
    SEQ(ModeManager::testModeRefusal(with("peer", 18899, false, {"http://192.168.1.177:8080"})), "");
    SEQ(ModeManager::testModeRefusal(with("server", 18080, true, {"http://192.168.1.177:8080"})), "");   // bridge is a peer setting

    // apply() refuses without touching the running generation
    Runtime rt;
    rt.registry.add("common", 10, [] { return std::unique_ptr<Module>(new RecModule("alpha")); }, "Alpha");
    rt.init();
    g_events.take();
    const int p = freePort();
    QVERIFY(rt.write(Runtime::cfg("peer", p, 1)));
    g_events.take();
    Config bad = Runtime::cfg("server", 8080, 2);
    QVERIFY(rt.write(bad));
    QVERIFY(g_events.take().empty());
    SEQ(rt.mm->mode(), "peer");
    QCOMPARE(rt.mm->port(), p);
    QCOMPARE(rt.mm->appliedRev(), int64_t(1));
    QVERIFY(!rt.mm->failed());
    SEQ(rt.mm->lastError(), "config refused: test mode refuses port 8080 (a live AN-FS port)");
    QCOMPARE(rt.fetch(p, "/api/alpha").status, 200);
    Config badBridge = Runtime::cfg("peer", p, 3);
    badBridge.bridgeUrls = {"http://192.168.1.177:8080"};
    QVERIFY(rt.write(badBridge));
    QVERIFY(g_events.take().empty());
    QCOMPARE(rt.mm->appliedRev(), int64_t(1));
    QVERIFY2(rt.mm->lastError().find("non-loopback bridge") != std::string::npos, rt.mm->lastError().c_str());
    // the next good config clears a refusal, even on the same key (no restart)
    QVERIFY(rt.write(Runtime::cfg("peer", p, 4)));
    QVERIFY(g_events.take().empty());             // same key: no restart
    QCOMPARE(rt.mm->appliedRev(), int64_t(4));
    QVERIFY2(rt.mm->lastError().empty(), "a same-key correction of a refused config clears the refusal text");
    QCOMPARE(rt.mm->lastErrorAtMs(), int64_t(0));
    QVERIFY(rt.write(Runtime::cfg("off", 0, 5)));
    QVERIFY(rt.mm->lastError().empty());
    // shutdown() is idempotent and the destructor never writes status: exercised by every fixture teardown
    rt.mm->shutdown();
    rt.mm->shutdown();
    QCOMPARE(rt.mm->mode(), std::string("off"));
}

// ---- status ------------------------------------------------------------------------------------------------------------

void TestAnfsCoreRuntime::statusWriterAndFields()
{
    // field order per mode
    Runtime rt;
    rt.registry.add("common", 10, [] { return std::unique_ptr<Module>(new RecModule("alpha")); }, "Alpha");
    rt.init();
    QVERIFY(rt.write(Runtime::cfg("off", 0, 7)));
    JVal off = buildStatus(*rt.mm, rt.store, rt.log, rt.started);
    QCOMPARE(joinKeys(keysOf(off)),
             QString("mode,version,ver,pid,nodeId,name,port,statusPort,listening,loopbackOnly,startedAt,updatedAt,"
                     "uptimeSec,uptimeS,rev,appliedRev,bridge,modules,lastError,lastErrorTs,configError,generation,config"));
    SEQ(off.getStr("mode"), "off");
    SEQ(off.getStr("version"), kVersion);
    SEQ(off.getStr("ver"), kVersion);
    QCOMPARE(off.getInt("pid"), int64_t(GetCurrentProcessId()));
    SEQ(off.getStr("nodeId"), "node-runtime-test");
    QVERIFY(off.get("listening").isNull());
    QVERIFY(off.get("bridge").isNull());
    QVERIFY(off.get("lastError").isNull() && off.get("lastErrorTs").isNull() && off.get("configError").isNull());
    QCOMPARE(off.getInt("rev"), int64_t(7));
    QCOMPARE(off.getInt("appliedRev"), int64_t(7));
    QVERIFY(off.get("modules").isObject() && off.get("modules").obj().empty());
    QVERIFY(json::parseIsoMs(off.getStr("startedAt")) > 0 && json::parseIsoMs(off.getStr("updatedAt")) > 0);
    QVERIFY(off.get("config").isObject());
    SEQ(off.get("config").getStr("mode"), "off");

    const int p = freePort();
    Config peer = Runtime::cfg("peer", p, 8);
    peer.agentToken = "runtime-test-token-value";
    peer.statusPort = 18897;
    peer.wan.passwordRecord = makePasswordRecord("test-only-password", 1000);
    QVERIFY(rt.write(peer));
    JVal on = buildStatus(*rt.mm, rt.store, rt.log, rt.started);
    QCOMPARE(joinKeys(keysOf(on)),
             QString("mode,version,ver,pid,nodeId,name,port,statusPort,listening,loopbackOnly,startedAt,updatedAt,"
                     "uptimeSec,uptimeS,rev,appliedRev,bridge,modules,lastError,lastErrorTs,configError,generation,gate,db,config"));
    SEQ(on.getStr("mode"), "peer");
    QCOMPARE(on.getInt("port"), int64_t(p));
    QCOMPARE(on.getInt("statusPort"), int64_t(18897));
    SEQ(json::stringify(on.get("listening")), std::string("[\"127.0.0.1:") + std::to_string(p) + "\"]");
    QCOMPARE(on.getBool("loopbackOnly"), false);
    QCOMPARE(on.getInt("generation"), int64_t(1));
    SEQ(on.getStr("gate"), "foundation");
    SEQ(on.get("db").getStr("path"), rt.dataDir + "\\anfs.db");
    SEQ(on.get("db").getStr("mode"), "peer");
    QVERIFY(on.get("db").get("sizeBytes").isInt());
    QCOMPARE(joinKeys(keysOf(on.get("modules"))), QString("alpha"));
    // secrets never appear
    const std::string text = json::stringify(on);
    QVERIFY(text.find("runtime-test-token-value") == std::string::npos);
    SEQ(on.get("config").getStr("agentToken"), "[set]");
    QVERIFY(text.find(peer.wan.passwordRecord->hash) == std::string::npos);

    // StatusWriter: timer on a ManualClock, extras, snapshot, the file, a failed write logged once
    {
        ManualClock mclock;
        ScopedClock sc(mclock);
        QTemporaryDir dir;
        const QString path = dir.path() + "/status.json";
        int builds = 0;
        Log log;
        log.setConsole(false);
        StatusWriter w(QDir::toNativeSeparators(path).toStdString(), [&] { return JVal::object({{"n", ++builds}}); }, &log);
        w.set("bridge", "connected");
        w.set("extra", JVal::object({{"a", 1}}));
        SEQ(json::stringify(w.snapshot()), "{\"n\":1,\"bridge\":\"connected\",\"extra\":{\"a\":1}}");
        w.start(2000);                            // writes at once
        QVERIFY(waitReal([&] { return QFileInfo::exists(path); }, 2000));
        JVal f;
        QVERIFY(json::tryParse(readAll(path), f));
        QCOMPARE(f.getInt("n"), int64_t(2));
        SEQ(f.getStr("bridge"), "connected");
        SEQ(readAll(path), json::stringifyPretty(f, 2));   // pretty, no trailing newline (C# Json.Serialize(d, true))
        mclock.advance(1990);
        QThread::msleep(60);
        QVERIFY(json::tryParse(readAll(path), f));
        QCOMPARE(f.getInt("n"), int64_t(2));      // not yet
        mclock.advance(10);
        QVERIFY(waitReal([&] { return json::tryParse(readAll(path), f) && f.getInt("n") == 3; }, 2000));
        w.remove("bridge");
        w.set("extra", JVal());                   // null removes (C# Set(key, null))
        w.writeNow();
        QVERIFY(json::tryParse(readAll(path), f));
        QCOMPARE(joinKeys(keysOf(f)), QString("n"));
        QCOMPARE(f.getInt("n"), int64_t(4));
        QElapsedTimer t;
        t.start();
        w.stop();
        QVERIFY(t.elapsed() < 1000);
        w.stop();                                 // idempotent
        QVERIFY(QDir(dir.path()).entryList({"*.tmp"}, QDir::Files).isEmpty());

        // a path under a regular file cannot be written: one warning, then silence until a success
        QFile blocker(dir.path() + "/blocker");
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        StatusWriter bad(QDir::toNativeSeparators(dir.path() + "/blocker/status.json").toStdString(), [] { return JVal::object(); }, &log);
        const int before = log.errorCount();
        bad.writeNow();
        bad.writeNow();
        bad.writeNow();
        log.flush();
        QCOMPARE(log.errorCount(), before);       // Warn, not Error
        // a builder that throws still produces a document
        StatusWriter thrower(QDir::toNativeSeparators(dir.path() + "/t.json").toStdString(), []() -> JVal { throw std::runtime_error("boom"); }, &log);
        SEQ(json::stringify(thrower.snapshot()), "{\"error\":\"status build failed: boom\"}");
    }

    // readStatusFile: missing -> null; a live pid with a fresh file -> running; a dead pid -> not running
    {
        QTemporaryDir dir;
        const std::string d = QDir::toNativeSeparators(dir.path()).toStdString();
        QVERIFY(readStatusFile(d).isNull());
        QVERIFY(writeFileAtomic(d + "\\status.json", json::stringify(JVal::object({{"mode", "off"}, {"pid", static_cast<int64_t>(GetCurrentProcessId())}}))));
        JVal s = readStatusFile(d);
        QCOMPARE(joinKeys(keysOf(s)), QString("mode,pid,ageSec,running"));
        QVERIFY(s.getBool("running"));
        QVERIFY(s.getInt("ageSec") < 5);
        // a pid that just exited (a finished `anfs version`) is not running
        qint64 deadPid = 0;
        {
            QProcess p;
            p.setProgram(QString::fromUtf8(ANFS_EXE_PATH));
            p.setArguments({"version"});
            p.start();
            QVERIFY(p.waitForStarted(10000));
            deadPid = p.processId();
            QVERIFY(p.waitForFinished(10000));
        }
        QVERIFY(deadPid > 0);
        QVERIFY(writeFileAtomic(d + "\\status.json", json::stringify(JVal::object({{"mode", "peer"}, {"pid", static_cast<int64_t>(deadPid)}}))));
        s = readStatusFile(d);
        QVERIFY(!s.getBool("running"));
        // a stale file (older than 20 s) is not running even for a live pid
        QVERIFY(writeFileAtomic(d + "\\status.json", json::stringify(JVal::object({{"mode", "off"}, {"pid", static_cast<int64_t>(GetCurrentProcessId())}}))));
        {
            ManualClock later(anfs::nowMs() + 25000);
            ScopedClock sc(later);
            s = readStatusFile(d);
            QVERIFY(!s.getBool("running"));
            QVERIFY(s.getInt("ageSec") >= 20);
        }
        QVERIFY(writeFileAtomic(d + "\\status.json", "{not json"));
        s = readStatusFile(d);
        QVERIFY(!s.getBool("running", true));
        QVERIFY2(s.getStr("error").find("unreadable status") == 0, s.getStr("error").c_str());
    }
}

// ---- the executable --------------------------------------------------------------------------------------------------------

void TestAnfsCoreRuntime::singleInstanceMutex()
{
    QTemporaryDir dir;
    const std::string d = QDir::toNativeSeparators(dir.path()).toStdString();
    std::string name;
    QVERIFY(acquireSingleInstance(d, &name));
    // C# Program.cs:137: "Global\anfs-" + BitConverter.ToString(SHA1(UTF8(dataDir.ToLowerInvariant()))).Replace("-","").Substring(0,16)
    std::string lower = d;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string expect = ids::sha1Hex(lower).substr(0, 16);
    for (char& c : expect) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    SEQ(name, "Global\\anfs-" + expect);
    QVERIFY(acquireSingleInstance(d));            // re-entrant for the owner
    // a second process on the same folder exits 3 at once
    Run second = runAnfs({"run", "--data", QString::fromStdString(d)}, 20000);
    QCOMPARE(second.exit, 3);
    QVERIFY2(second.err.find("anfs is already running for " + d) != std::string::npos, second.err.c_str());
    // the same folder spelled differently (trailing slash, case) is the same instance: GetFullPath + lower-case
    Run third = runAnfs({"run", "--data", QString::fromStdString(d + "\\").toUpper()}, 20000);
    QCOMPARE(third.exit, 3);
    releaseSingleInstance();
    releaseSingleInstance();                      // idempotent
    QVERIFY(acquireSingleInstance(d));
    releaseSingleInstance();
}

void TestAnfsCoreRuntime::cliRunStatusEndpointAndConfigSet()
{
    QTemporaryDir dir;
    const std::string d = QDir::toNativeSeparators(dir.path()).toStdString() + "\\data";
    const QString qd = QString::fromStdString(d);
    const int statusPort = freePort();
    const int peerPort = freePort();
    QVERIFY(statusPort > 0 && peerPort > 0 && statusPort != peerPort);

    // config set creates the folder and a default config (mode off, fresh nodeId, machine name), rev 1
    Run r = runAnfs({"config", "set", "statusPort=" + QString::number(statusPort), "--data", qd});
    QCOMPARE(r.exit, 0);
    SEQ(r.out, "{\"rev\":1,\"mode\":\"off\"}\n");
    Config saved = Config::fromJson(readAll(qd + "\\anfs.json"));
    SEQ(saved.mode, "off");
    QCOMPARE(saved.statusPort, statusPort);
    QCOMPARE(saved.rev, int64_t(1));
    QCOMPARE(saved.nodeId.size(), size_t(32));
    QVERIFY(!saved.name.empty());
    // status before run: no status file -> exit 1
    r = runAnfs({"status", "--data", qd});
    QCOMPARE(r.exit, 1);
    SEQ(r.out, "{\"running\":false,\"mode\":\"unknown\",\"error\":\"no status file\"}\n");

    Child child;
    QVERIFY(child.start(qd));
    const qint64 pid = child.p.processId();
    QVERIFY(waitReal([&] { JVal s = statusFile(d); return s.isObject() && s.getInt("pid") == pid && s.getStr("mode") == "off"; }, 20000));
    JVal s = statusFile(d);
    QCOMPARE(s.getInt("appliedRev"), int64_t(1));
    QCOMPARE(s.getInt("statusPort"), int64_t(statusPort));
    QVERIFY(QFileInfo::exists(qd + "\\logs"));

    // anfs status: running, ageSec
    r = runAnfs({"status", "--data", qd});
    QCOMPARE(r.exit, 0);
    JVal st;
    QVERIFY(json::tryParse(r.out, st));
    QVERIFY(st.getBool("running"));
    QCOMPARE(st.getInt("pid"), int64_t(pid));
    QVERIFY(st.get("ageSec").isInt());

    // the loopback status endpoint
    http::HttpClient client;
    http::HttpClient::Result ep = client.fetch("GET", "http://127.0.0.1:" + std::to_string(statusPort) + "/status", {}, {}, 5000);
    QCOMPARE(ep.status, 200);
    SEQ(ep.headers.get("content-type"), "application/json; charset=utf-8");
    JVal e;
    QVERIFY(json::tryParse(ep.body, e));
    SEQ(e.getStr("mode"), "off");
    QCOMPARE(e.getInt("pid"), int64_t(pid));
    QVERIFY(e.get("listening").isNull());
    ep = client.fetch("GET", "http://127.0.0.1:" + std::to_string(statusPort) + "/api/anything", {}, {}, 5000);
    QCOMPARE(ep.status, 404);

    // config set applies live: the watcher (2 s) -> ModeManager; appliedRev follows rev, the peer port opens
    r = runAnfs({"config", "set", "mode=peer", "port=" + QString::number(peerPort), "bindAll=false", "test=true",
                 "name=cli-test", "folders.dropbox=C:\\filedrop", "corsAllow=a.example, b.example", "myModule.flag=true",
                 "--data", qd});
    QCOMPARE(r.exit, 0);
    SEQ(r.out, "{\"rev\":2,\"mode\":\"peer\"}\n");
    saved = Config::fromJson(readAll(qd + "\\anfs.json"));
    SEQ(saved.dropbox(), "C:\\filedrop");
    QCOMPARE(saved.corsAllow.size(), size_t(2));
    QCOMPARE(saved.rawGet("myModule").getBool("flag"), true);
    QVERIFY(waitReal([&] { JVal x = statusFile(d); return x.getInt("appliedRev") == 2 && x.getStr("mode") == "peer"; }, 15000));
    s = statusFile(d);
    QCOMPARE(s.getInt("port"), int64_t(peerPort));
    SEQ(json::stringify(s.get("listening")), std::string("[\"127.0.0.1:") + std::to_string(peerPort) + "\"]");
    SEQ(s.get("config").getStr("name"), "cli-test");
    ep = client.fetch("GET", "http://127.0.0.1:" + std::to_string(peerPort) + "/api/ping", {}, {}, 5000);
    QCOMPARE(ep.status, 404);                     // zero modules: the router's default answer
    SEQ(ep.body, "{\"error\":\"Not found\"}");

    // the test-mode guard at the writer: a live port is refused and nothing changes
    r = runAnfs({"config", "set", "port=8080", "--data", qd});
    QCOMPARE(r.exit, 2);
    QVERIFY2(r.err.find("test mode refuses port 8080") != std::string::npos, r.err.c_str());
    r = runAnfs({"config", "set", "bridgeUrl=http://192.168.1.177:8080", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "set", "mode=relay", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "set", "port=70000", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "set", "port=80", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "set", "novalue", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "frobnicate", "--data", qd});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"config", "get", "--data", qd});
    QCOMPARE(r.exit, 0);
    JVal g;
    QVERIFY(json::tryParse(r.out, g));
    QCOMPARE(g.getInt("rev"), int64_t(2));
    QCOMPARE(g.getInt("port"), int64_t(peerPort));

    // set-wan-record
    const PasswordRecord rec = makePasswordRecord("test-only-password", 1000);
    r = runAnfs({"config", "set-wan-record", QString::fromStdString(b64::encode(json::stringify(rec.toJval()))), "--data", qd});
    QCOMPARE(r.exit, 0);
    SEQ(r.out, "{\"rev\":3,\"passwordRecord\":\"[set]\"}\n");
    saved = Config::fromJson(readAll(qd + "\\anfs.json"));
    QVERIFY(saved.wan.passwordRecord && verifyPassword(*saved.wan.passwordRecord, "test-only-password"));
    r = runAnfs({"config", "set-wan-record", "bm90IGpzb24=", "--data", qd});   // "not json"
    QCOMPARE(r.exit, 2);

    // back to off: the peer port closes, appliedRev follows
    r = runAnfs({"config", "set", "mode=off", "--data", qd});
    QCOMPARE(r.exit, 0);
    SEQ(r.out, "{\"rev\":4,\"mode\":\"off\"}\n");
    QVERIFY(waitReal([&] { JVal x = statusFile(d); return x.getInt("appliedRev") == 4 && x.getStr("mode") == "off"; }, 15000));
    QVERIFY(waitReal([&] { return client.fetch("GET", "http://127.0.0.1:" + std::to_string(peerPort) + "/", {}, {}, 2000).status == 0; }, 5000));
    ep = client.fetch("GET", "http://127.0.0.1:" + std::to_string(statusPort) + "/status", {}, {}, 5000);
    QCOMPARE(ep.status, 200);                     // the status endpoint stays up in mode off

    // an invalid file is reported as configError and the previous settings stay
    QVERIFY(writeFileAtomic(d + "\\anfs.json", "{\"mode\":\"relay\"}"));
    QVERIFY(waitReal([&] { JVal x = statusFile(d); return x.get("configError").isString(); }, 15000));
    s = statusFile(d);
    QVERIFY2(s.getStr("configError").find("mode") != std::string::npos, s.getStr("configError").c_str());
    SEQ(s.getStr("mode"), "off");
    QCOMPARE(s.getInt("appliedRev"), int64_t(4));

    // killed hard (StarRoot style): the mutex is free again and `status` says not running
    child.p.kill();
    QVERIFY(child.p.waitForFinished(10000));
    QVERIFY(acquireSingleInstance(d));
    releaseSingleInstance();
    r = runAnfs({"status", "--data", qd});
    QCOMPARE(r.exit, 1);
    QVERIFY(json::tryParse(r.out, st));
    QVERIFY(!st.getBool("running", true));
    QCOMPARE(st.getInt("pid"), int64_t(pid));
}

void TestAnfsCoreRuntime::cliConfigSetBumpsRevAtomically()
{
    QTemporaryDir dir;
    const QString qd = QDir::toNativeSeparators(dir.path());
    Run r = runAnfs({"config", "set", "name=seed", "--data", qd});
    QCOMPARE(r.exit, 0);
    const int n = 8;
    std::vector<std::unique_ptr<QProcess>> ps;
    for (int i = 0; i < n; ++i) {
        auto p = std::make_unique<QProcess>();
        p->setProgram(QString::fromUtf8(ANFS_EXE_PATH));
        p->setArguments({"config", "set", QString("k%1=%1").arg(i), "name=writer-" + QString::number(i), "--data", qd});
        p->start();
        QVERIFY(p->waitForStarted(10000));
        ps.push_back(std::move(p));
    }
    for (auto& p : ps) {
        QVERIFY(p->waitForFinished(60000));
        QVERIFY2(p->exitCode() == 0, p->readAllStandardError().constData());
    }
    Config c = Config::fromJson(readAll(qd + "\\anfs.json"));
    QCOMPARE(c.rev, int64_t(1 + n));              // every writer's bump landed exactly once
    for (int i = 0; i < n; ++i) QCOMPARE(c.rawGet("k" + std::to_string(i)).asInt(-1), int64_t(i));
    QVERIFY(QDir(qd).entryList({"*.tmp"}, QDir::Files).isEmpty());
}

void TestAnfsCoreRuntime::cliHashPasswordAndUsage()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ANFS_PASSWORD", "test-only-password");
    Run r = runAnfs({"hash-password", "--iterations", "1000"}, 30000, env);
    QCOMPARE(r.exit, 0);
    JVal j;
    QVERIFY(json::tryParse(r.out, j));
    QCOMPARE(joinKeys(keysOf(j)), QString("algo,iterations,salt,hash"));
    QCOMPARE(j.getInt("iterations"), int64_t(1000));
    std::optional<PasswordRecord> rec = PasswordRecord::fromJval(j);
    QVERIFY(rec && verifyPassword(*rec, "test-only-password"));
    QVERIFY(!verifyPassword(*rec, "test-only-wrong"));
    QVERIFY(r.out.find("test-only-password") == std::string::npos);
    env.insert("ANFS_PASSWORD", "tiny");
    r = runAnfs({"hash-password"}, 30000, env);
    QCOMPARE(r.exit, 2);
    QVERIFY(r.out.empty() && r.err.find("tiny") == std::string::npos && r.err.find("too short") != std::string::npos);
    env.remove("ANFS_PASSWORD");
    r = runAnfs({"hash-password"}, 30000, env);
    QCOMPARE(r.exit, 2);
    r = runAnfs({"hash-password", "--iterations", "0"}, 30000, env);
    QCOMPARE(r.exit, 2);

    r = runAnfs({"version"});
    QCOMPARE(r.exit, 0);
    SEQ(r.out, std::string(versionLine()) + "\n");
    r = runAnfs({"VERSION", "--verbose"});
    QCOMPARE(r.exit, 0);
    QVERIFY(r.out.find("cpp, MSVC") != std::string::npos);
    r = runAnfs({});
    QCOMPARE(r.exit, 0);                          // C#: no command prints usage and exits 0
    QVERIFY(r.err.find("usage: anfs") != std::string::npos);
    r = runAnfs({"bogus"});
    QCOMPARE(r.exit, 2);
    r = runAnfs({"status", "--data"});
    QCOMPARE(r.exit, 2);
    QVERIFY(r.err.find("--data needs a value") != std::string::npos);
    r = runAnfs({"status", "--what"});
    QCOMPARE(r.exit, 2);
    QVERIFY(r.err.find("unknown argument: --what") != std::string::npos);
    for (const char* later : {"import", "install", "uninstall", "selftest", "peer-legacy", "shell"}) {
        r = runAnfs({later});
        QCOMPARE(r.exit, 2);
        QVERIFY2(r.err.find("not implemented in this build") != std::string::npos, later);
    }
}

// ---- ANFS-F8: testkit, sim, harness ------------------------------------------------------------------------------------

namespace {

// A module for ModuleHost: maps one route and records its lifecycle in g_events.
class TkModule : public Module {
public:
    const char* name() const override { return "tk-probe"; }
    void start(Context& ctx) override
    {
        g_events.add("start tk-probe");
        ctx.http.get("/api/tk", [](http::Ctx& c) { c.json(200, JVal::object({{"ok", true}, {"who", "tk-probe"}})); }, http::kRouteNone, name());
    }
    void stop() override { g_events.add("stop tk-probe"); }
    JVal status() const override { return JVal::object({{"probe", true}}); }
};

std::string simExePath()
{
    QFileInfo fi(QString::fromUtf8(ANFS_EXE_PATH));
    return QDir::toNativeSeparators(fi.absoluteDir().filePath(QStringLiteral("anfs_sim.exe"))).toStdString();
}

struct RunResult {
    int exit = -1;
    std::string out;
    std::string err;
};

RunResult runProcess(const std::string& exe, const QStringList& args, int timeoutMs = 60000)
{
    QProcess p;
    p.setProgram(QString::fromStdString(exe));
    p.setArguments(args);
    p.start();
    RunResult r;
    if (!p.waitForStarted(10000)) {
        r.err = "did not start";
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(5000);
        r.err = "timeout";
        return r;
    }
    r.exit = p.exitCode();
    auto lf = [](QByteArray b) { b.replace('\r', ""); return b.toStdString(); };
    r.out = lf(p.readAllStandardOutput());
    r.err = lf(p.readAllStandardError());
    return r;
}

} // namespace

void TestAnfsCoreRuntime::testkitServerAndRawClient()
{
    testkit::TestServer ts;
    ts.router.get("/api/ping", [](http::Ctx& c) { c.json(200, JVal::object({{"ok", true}, {"n", 1}})); });
    ts.router.post("/api/echo", [](http::Ctx& c) {
        std::string body = c.body.readAll(1 << 20);
        c.res.begin(200, -1);                       // chunked: the raw client must see the framing
        c.res.write(body);
        c.res.end();
    });
    ts.start();
    QVERIFY(ts.port() > 0);
    QVERIFY(ts.port() != 18080 && ts.port() != 8080);

    // WinHTTP path
    auto r = ts.fetch("GET", "/api/ping");
    QCOMPARE(r.status, 200);
    ANFS_SEQ(r.body, "{\"ok\":true,\"n\":1}");

    // Raw path: exact status line, header order, body; two pipelined requests on one connection.
    testkit::RawClient raw(ts.port());
    QVERIFY(raw.connected());
    raw.request("GET", "/api/ping");
    raw.request("POST", "/api/echo", {{"Content-Type", "text/plain"}}, "hello-raw");
    testkit::RawResponse a = raw.readResponse();
    QVERIFY(a.complete);
    QCOMPARE(a.status, 200);
    ANFS_SEQ(a.reason, "OK");
    ANFS_SEQ(a.version, "HTTP/1.1");
    QVERIFY(a.raw.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    ANFS_SEQ(a.header("Content-Type"), "application/json; charset=utf-8");
    ANFS_SEQ(a.header("content-length"), "17");
    ANFS_SEQ(a.body, "{\"ok\":true,\"n\":1}");
    QVERIFY(!a.has("X-Powered-By"));
    testkit::RawResponse b = raw.readResponse();
    QVERIFY(b.complete);
    QCOMPARE(b.status, 200);
    QVERIFY(b.chunked());
    ANFS_SEQ(b.body, "hello-raw");
    QVERIFY2(b.raw.find("9\r\nhello-raw\r\n0\r\n\r\n") != std::string::npos, b.raw.c_str());
    // HEAD: head only
    raw.request("HEAD", "/api/ping");
    testkit::RawResponse h = raw.readResponse(true);
    QCOMPARE(h.status, 200);
    ANFS_SEQ(h.header("Content-Length"), "17");
    QVERIFY(h.body.empty());
    raw.reset();

    // PortHolder makes a bind fail; freePort hands out a port nobody holds.
    int fp = testkit::freePort();
    QVERIFY(fp > 0);
    testkit::PortHolder hold(fp);
    QCOMPARE(hold.port(), fp);
    {
        http::Router r2;
        http::Server s2(r2, [] { return std::make_shared<const Config>(); });
        QVERIFY_THROWS_EXCEPTION(http::BindError, s2.start(fp, false, "127.0.0.1"));
    }
    hold.release();
    ts.stop();
    QVERIFY(!ts.server.running());

    // TempDir helpers
    testkit::TempDir td;
    QVERIFY(td.valid());
    QVERIFY(!td.write("a\\b.txt", "xyz").empty());
    ANFS_SEQ(td.read("a\\b.txt"), "xyz");
    QVERIFY(td.read("missing").empty());
}

void TestAnfsCoreRuntime::testkitFakePeerAndMemProbe()
{
    testkit::FakePeer peer;
    QVERIFY(peer.port() > 0);
    peer.onGet("/api/health", testkit::FakeResponse::json(200, "{\"ok\":true}"));
    testkit::FakeResponse slow = testkit::FakeResponse::text(200, "0123456789");
    slow.chunked = true;
    slow.chunkSize = 4;
    slow.bodyDelayMs = 50;
    peer.onGet("/slow/*", slow);
    testkit::FakeResponse rst;
    rst.abortAfterHead = true;
    rst.status = 200;
    rst.body = "never";
    peer.onGet("/abort", rst);
    testkit::FakeResponse hard;
    hard.abortBeforeHead = true;
    peer.onPost("/rst", hard);

    http::HttpClient client;
    auto r = client.fetch("GET", peer.url("/api/health?x=1"), {}, {}, 3000);
    QCOMPARE(r.status, 200);
    ANFS_SEQ(r.body, "{\"ok\":true}");
    r = client.fetch("GET", peer.url("/slow/thing"), {}, {}, 3000);
    QCOMPARE(r.status, 200);
    ANFS_SEQ(r.body, "0123456789");
    r = client.fetch("GET", peer.url("/nope"), {}, {}, 3000);
    QCOMPARE(r.status, 404);
    ANFS_SEQ(r.body, "{\"error\":\"Not found\"}");
    http::Headers hs;
    hs.add("X-Test", "yes");
    r = client.fetch("POST", peer.url("/rst"), hs, "body!", 3000);
    QCOMPARE(r.status, 0);                        // RST before the head: a transport error
    QVERIFY(!r.error.empty());

    // The raw client sees the abort after the head.
    testkit::RawClient raw(peer.port());
    raw.request("GET", "/abort");
    testkit::RawResponse a = raw.readResponse();
    QCOMPARE(a.status, 200);
    QVERIFY(!a.complete);
    raw.close();

    QVERIFY(peer.waitForRequests(5));
    auto reqs = peer.requests();
    QCOMPARE(reqs.size(), size_t(5));
    ANFS_SEQ(reqs[0].method, "GET");
    ANFS_SEQ(reqs[0].target, "/api/health?x=1");
    ANFS_SEQ(reqs[0].path, "/api/health");
    ANFS_SEQ(reqs[3].method, "POST");
    ANFS_SEQ(reqs[3].body, "body!");
    ANFS_SEQ(reqs[3].header("x-test"), "yes");
    QVERIFY(peer.accepts() >= 2);

    // Dynamic handler before the table.
    peer.setHandler([](const testkit::FakeRequest& q) {
        if (q.path == "/dyn") return testkit::FakeResponse::text(201, "made:" + q.header("X-N"));
        return testkit::FakeResponse{0};
    });
    hs.clear();
    hs.add("X-N", "7");
    r = client.fetch("GET", peer.url("/dyn"), hs, {}, 3000);
    QCOMPARE(r.status, 201);
    ANFS_SEQ(r.body, "made:7");
    r = client.fetch("GET", peer.url("/api/health"), {}, {}, 3000);
    QCOMPARE(r.status, 200);
    peer.stop();
    peer.stop();                                  // idempotent

    // MemProbe: a 64 MB allocation moves private bytes by about that much; freed, the peak stays.
    testkit::MemProbe probe;
    QVERIFY(probe.flat(1 << 20));
    {
        std::vector<char> big(64u << 20, 1);
        QVERIFY2(probe.privateDelta() > (48 << 20), probe.describe().c_str());
        QVERIFY(!probe.flat(32 << 20));
    }
    QVERIFY2(probe.privateDelta() < (16 << 20), probe.describe().c_str());
    QVERIFY(!probe.describe().empty());
    probe.restart();
    QVERIFY(probe.flat(1 << 20));
}

void TestAnfsCoreRuntime::testkitModuleHost()
{
    g_events.take();
    testkit::ModuleHost host;
    QVERIFY(host.tmp.valid());
    host.add<TkModule>("peer", 50);
    host.init();
    QCOMPARE(host.port(), 0);
    QVERIFY(host.write(testkit::ModuleHost::cfg("peer", 0, 1)));
    QVERIFY(host.mm->appliedRev() == 1);
    QVERIFY(host.port() > 0);
    QVERIFY2(host.port() != 18899 && host.port() != 8899, "ephemeral port expected");
    auto r = host.fetch("/api/tk");
    QCOMPARE(r.status, 200);
    ANFS_SEQ(r.body, "{\"ok\":true,\"who\":\"tk-probe\"}");
    JVal st = host.statusDoc();
    ANFS_SEQ(st.get("mode").asString(), "peer");
    QVERIFY(st.get("modules").get("tk-probe").get("probe").isBool());
    QVERIFY(QFileInfo::exists(QString::fromStdString(host.store.path())));
    host.apply(testkit::ModuleHost::cfg("off", 0, 2));
    QCOMPARE(host.port(), 0);
    auto ev = g_events.take();
    QCOMPARE(ev.size(), size_t(2));
    ANFS_SEQ(ev[0], "start tk-probe");
    ANFS_SEQ(ev[1], "stop tk-probe");
}

void TestAnfsCoreRuntime::simGuardsAndBridge()
{
    std::string sim = simExePath();
    if (!QFileInfo::exists(QString::fromStdString(sim))) QSKIP("anfs_sim.exe not built next to anfs.exe");
    RunResult r = runProcess(sim, {"version"});
    QCOMPARE(r.exit, 0);
    QVERIFY(r.out.find(kVersion) != std::string::npos);
    r = runProcess(sim, {});
    QCOMPARE(r.exit, 2);
    for (const char* port : {"8080", "8899", "443", "7700"}) {
        r = runProcess(sim, {"bridge", "--port", port});
        QVERIFY2(r.exit == 2 && r.err.find("refused port") != std::string::npos, (std::string(port) + ": " + r.err).c_str());
    }
    r = runProcess(sim, {"peer", "--bridge", "http://192.168.1.177:18080", "--port", "18897"});
    QVERIFY2(r.exit == 2 && r.err.find("refused host 192.168.1.177") != std::string::npos, r.err.c_str());
    r = runProcess(sim, {"peer", "--bridge", "https://example.invalid", "--port", "18897"});
    QVERIFY2(r.exit == 2 && r.err.find("refused host example.invalid") != std::string::npos, r.err.c_str());
    r = runProcess(sim, {"peer", "--bridge", "http://192.168.1.5:18080", "--port", "18897"});
    QVERIFY2(r.exit == 2 && r.err.find("loopback only") != std::string::npos, r.err.c_str());
    QTemporaryDir outDir;
    r = runProcess(sim, {"record", "--target", "http://127.0.0.1:8080", "--out", QDir::toNativeSeparators(outDir.path())});
    QVERIFY2(r.exit == 2 && r.err.find("refused port 8080") != std::string::npos, r.err.c_str());

    // A bridge sim on an ephemeral port: health, register (md5 id), a 200 ms park then 204; stopped by its PID.
    int port = freePort();
    QVERIFY(port > 0);
    QProcess bridge;
    bridge.setProgram(QString::fromStdString(sim));
    bridge.setArguments({"bridge", "--port", QString::number(port), "--park-ms", "200"});
    bridge.start();
    QVERIFY(bridge.waitForStarted(10000));
    http::HttpClient client;
    std::string base = "http://127.0.0.1:" + std::to_string(port);
    QVERIFY(testkit::waitUntil([&] { return client.fetch("GET", base + "/api/health", {}, {}, 500).status == 200; }, 10000));
    auto h = client.fetch("GET", base + "/api/health", {}, {}, 3000);
    ANFS_SEQ(h.body, std::string("{\"ok\":true,\"sim\":\"bridge\",\"version\":\"") + kVersion + "\"}");
    http::Headers js;
    js.add("Content-Type", "application/json");
    auto reg = client.fetch("POST", base + "/api/agents/register", js, "{\"name\":\"PC1\",\"ip\":\"192.168.1.20\"}", 3000);
    QCOMPARE(reg.status, 200);
    ANFS_SEQ(reg.body, "{\"ok\":true,\"id\":\"cb0bff524238\"}");   // the ids.h vector
    QElapsedTimer t;
    t.start();
    auto poll = client.fetch("GET", base + "/api/agents/poll", {}, {}, 5000);
    QCOMPARE(poll.status, 204);
    QVERIFY2(t.elapsed() >= 150, qPrintable(QString::number(t.elapsed())));
    qint64 pid = bridge.processId();
    QVERIFY(pid > 0);
    bridge.kill();                                // only the PID we launched
    QVERIFY(bridge.waitForFinished(5000));
    QVERIFY(testkit::waitUntil([&] { return client.fetch("GET", base + "/api/health", {}, {}, 300).status == 0; }, 5000));
}

void TestAnfsCoreRuntime::harnessGuardsAndSmoke()
{
    std::string pwsh = testkit::pwshPath();
    if (pwsh.empty()) QSKIP("pwsh not installed");
    QString script = QDir::toNativeSeparators(QString::fromUtf8(ANFS_REPO_ROOT) + "/tools/anfs/test/harness/run-compat.ps1");
    QVERIFY2(QFileInfo::exists(script), qPrintable(script));
    QStringList common = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
                          "-AnfsExe", QString::fromUtf8(ANFS_EXE_PATH)};

    // -DryRun refuses the live ports and hosts (exit 2, the guard text names the offender) and starts nothing.
    struct Bad {
        QStringList args;
        const char* text;
    };
    const Bad bad[] = {
        {{"-CppServerPort", "8080"}, "8080 belongs to the live stack"},
        {{"-CppPeerPort", "8899"}, "8899 belongs to the live stack"},
        {{"-SimBridgePort", "443"}, "443 belongs to the live stack"},
        {{"-CsServerPort", "7700"}, "7700 belongs to the live stack"},
        {{"-BridgeUrl", "http://192.168.1.177:8080"}, "192.168.1.177 is the live Node bridge"},
        {{"-BridgeUrl", "https://example.invalid"}, "example.invalid is the live Node bridge"},
        {{"-BridgeUrl", "http://192.168.1.9:18080"}, "is not loopback"},
    };
    for (const Bad& b : bad) {
        RunResult r = runProcess(pwsh, common + b.args + QStringList{"-DryRun"}, 60000);
        QVERIFY2(r.exit == 2, (b.text + std::string(": exit ") + std::to_string(r.exit) + " " + r.out + r.err).c_str());
        QVERIFY2(r.out.find(b.text) != std::string::npos, (b.text + std::string(" missing in: ") + r.out + r.err).c_str());
        QVERIFY(r.out.find("[refused] guards") != std::string::npos);
    }
    // A clean dry run passes the guards, writes the configs and the report, starts nothing.
    RunResult ok = runProcess(pwsh, common + QStringList{"-DryRun"}, 60000);
    QVERIFY2(ok.exit == 0, (ok.out + ok.err).c_str());
    QVERIFY(ok.out.find("[ok] guards") != std::string::npos);
    QVERIFY(ok.out.find("[skipped] start - dry run") != std::string::npos);

    // Smoke: starts anfs.exe on 18080 by PID, waits for `anfs status` running, GETs once, stops it by PID.
    if (testkit::PortHolder probe(18080); probe.port() == 0) QSKIP("18080 is busy on this machine (another AN-FS test run)");
    RunResult smoke = runProcess(pwsh, common + QStringList{"-SmokeOnly"}, 90000);
    if (smoke.exit == 2 && smoke.out.find("is held by PID") != std::string::npos) QSKIP("18080 taken between the probe and the run");
    QVERIFY2(smoke.exit == 0, (smoke.out + smoke.err).c_str());
    QVERIFY2(smoke.out.find("[ok] start cpp-server - pid ") != std::string::npos, smoke.out.c_str());
    QVERIFY2(smoke.out.find("[ok] cpp-server running - status running, port 18080 owned by pid ") != std::string::npos, smoke.out.c_str());
    QVERIFY2(smoke.out.find("[ok] GET /api/health - HTTP ") != std::string::npos, smoke.out.c_str());
    QVERIFY2(smoke.out.find("[ok] stop cpp-server (pid ") != std::string::npos && smoke.out.find("- stopped") != std::string::npos, smoke.out.c_str());
    // The port is free again: nothing of ours is left behind.
    testkit::PortHolder after(18080);
    QVERIFY2(after.port() == 18080, "18080 still held after the harness stopped its anfs.exe");
}

QTEST_GUILESS_MAIN(TestAnfsCoreRuntime)
#include "test_anfs_core_runtime.moc"
