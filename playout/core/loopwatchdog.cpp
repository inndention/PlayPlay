#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif
#include "loopwatchdog.h"
#include "healthendpoint.h"
#include "minidump.h"
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

qint64 steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- activity tag: process-wide, written by the main thread, read by the monitor and /healthz threads ---------------
// A static tag is one pointer store. A copied tag uses a seqlock so readers never see a half-written string and the
// writer never waits.
std::atomic<const char*> g_tag{nullptr};
std::atomic<bool> g_textActive{false};
std::atomic<quint32> g_seq{0};
char g_text[96];
#ifdef _WIN32
std::atomic<DWORD> g_mainTid{0};
bool onMainThread() { const DWORD t = g_mainTid.load(std::memory_order_relaxed); return t == 0 || t == GetCurrentThreadId(); }
#else
bool onMainThread() { return true; }
#endif

void writeText(const char* s, size_t n) {
    if (n > sizeof(g_text) - 1) {
        n = sizeof(g_text) - 1;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // never cut a UTF-8 sequence
    }
    const quint32 q = g_seq.load(std::memory_order_relaxed);
    g_seq.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(g_text, s, n);
    g_text[n] = 0;
    g_seq.store(q + 2, std::memory_order_release);
    g_textActive.store(true, std::memory_order_release);
}

std::string readActivity() {
    if (g_textActive.load(std::memory_order_acquire)) {
        for (int tries = 0; tries < 64; ++tries) {
            const quint32 s1 = g_seq.load(std::memory_order_acquire);
            if (s1 & 1) { std::this_thread::yield(); continue; }
            char tmp[sizeof(g_text)];
            std::memcpy(tmp, g_text, sizeof(tmp));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_seq.load(std::memory_order_relaxed) != s1) continue;
            tmp[sizeof(tmp) - 1] = 0;
            return std::string(tmp);
        }
        return std::string();
    }
    const char* t = g_tag.load(std::memory_order_acquire);
    return t ? std::string(t) : std::string();
}

// --- JSON line builder (no Qt, no locale: numbers are integers or fixed one-decimal) --------------------------------
void jsonEscape(std::string& out, const std::string& s) {
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += ch;
    }
}

struct Json {
    std::string s = "{";
    Json& key(const char* k) { if (s.size() > 1) s += ','; s += '"'; s += k; s += "\":"; return *this; }
    Json& str(const char* k, const std::string& v) { key(k); s += '"'; jsonEscape(s, v); s += '"'; return *this; }
    Json& num(const char* k, long long v) { key(k); s += std::to_string(v); return *this; }
    Json& tenths(const char* k, double v) {
        const long long t = static_cast<long long>(v * 10.0 + (v >= 0 ? 0.5 : -0.5));
        key(k); s += std::to_string(t / 10) + "." + std::to_string(t < 0 ? -(t % 10) : t % 10); return *this;
    }
    Json& boolean(const char* k, bool v) { key(k); s += v ? "true" : "false"; return *this; }
    Json& strings(const char* k, const std::vector<std::string>& v) {
        key(k); s += '[';
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ','; s += '"'; jsonEscape(s, v[i]); s += '"'; }
        s += ']'; return *this;
    }
    std::string done() const { return s + "}"; }
};

std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }

std::string utcIsoNow() {
#ifdef _WIN32
    SYSTEMTIME t; GetSystemTime(&t);
    char b[40];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
#else
    return utf8(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
#endif
}

std::string localDate() {
#ifdef _WIN32
    SYSTEMTIME t; GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return b;
#else
    return utf8(QDate::currentDate().toString(Qt::ISODate));
#endif
}

struct ProcInfo { double privateMB = 0; long long handles = 0, threads = 0; };

ProcInfo procInfo() {
    ProcInfo p;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        p.privateMB = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
    DWORD hc = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &hc)) p.handles = hc;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{}; te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == pid) ++p.threads;
        CloseHandle(snap);
    }
#endif
    return p;
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define PP_WATCHDOG_STACKS 1
// The main thread is suspended only for GetThreadContext and a memcpy of its used stack: no locks (heap, loader,
// DbgHelp) are taken while it is stopped, so it cannot deadlock with whatever it holds. No C++ objects here (SEH).
DWORD captureRaw(HANDLE th, DWORD64 stackBase, CONTEXT* ctx, unsigned char* buf, size_t cap, size_t* len) {
    *len = 0;
    if (SuspendThread(th) == DWORD(-1)) return 1;
    DWORD rc = 0;
    ctx->ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, ctx)) rc = 2;
    else {
        const DWORD64 rsp = ctx->Rsp;
        const size_t n = stackBase > rsp ? size_t(std::min<DWORD64>(stackBase - rsp, cap)) : 0;
        __try { std::memcpy(buf, reinterpret_cast<const void*>(rsp), n); *len = n; }
        __except (EXCEPTION_EXECUTE_HANDLER) { *len = 0; rc = 3; }
    }
    ResumeThread(th);
    return rc;
}

inline void reloc(DWORD64& r, DWORD64 lo, DWORD64 hi, DWORD64 delta) { if (r >= lo && r < hi) r += delta; }

// Unwinds the copied stack: registers that point into the original stack range are moved into the copy, so
// RtlVirtualUnwind reads the snapshot, not the (now running again) live stack. Any bad read ends the walk (SEH).
int unwindCopy(CONTEXT* ctx, DWORD64 origRsp, const unsigned char* buf, size_t len, quint64* pcs, int maxPcs) {
    int n = 0;
    const DWORD64 lo = origRsp, hi = origRsp + len, delta = DWORD64(reinterpret_cast<quintptr>(buf)) - origRsp;
    const DWORD64 copyLo = lo + delta, copyHi = hi + delta;
    __try {
        pcs[n++] = ctx->Rip;
        while (n < maxPcs) {
            reloc(ctx->Rsp, lo, hi, delta); reloc(ctx->Rbp, lo, hi, delta); reloc(ctx->Rbx, lo, hi, delta);
            reloc(ctx->Rsi, lo, hi, delta); reloc(ctx->Rdi, lo, hi, delta); reloc(ctx->R12, lo, hi, delta);
            reloc(ctx->R13, lo, hi, delta); reloc(ctx->R14, lo, hi, delta); reloc(ctx->R15, lo, hi, delta);
            if (ctx->Rsp < copyLo || ctx->Rsp + 8 > copyHi) break;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
            if (!f) {                                   // leaf function: the return address is at [rsp]
                ctx->Rip = *reinterpret_cast<const DWORD64*>(ctx->Rsp);
                ctx->Rsp += 8;
            } else {
                PVOID handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, f, ctx, &handlerData, &establisher, nullptr);
            }
            if (ctx->Rip == 0) break;
            pcs[n++] = ctx->Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
#endif

LoopWatchdog* s_instance = nullptr;

const char* stateName(int s) { return s == LoopWatchdog::Hang ? "hang" : s == LoopWatchdog::Stall ? "stall" : "ok"; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

struct LoopWatchdog::Impl {
    LoopWatchdogOptions o;
    LoopWatchdog* q = nullptr;
    QTimer timer;
    std::thread monitor;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<bool> exiting{false};
    std::atomic<bool> firstBeat{false};
    std::atomic<qint64> lastBeat{0}, maxLag{0}, curBucketMax{0}, prevBucketMax{0}, bucketId{0}, maxGapPending{0};
    std::atomic<int> state{Ok};
    std::atomic<quint64> stalls{0}, hangs{0}, dumps{0};
    qint64 startMs = 0;
    std::string nameU8, fileNameU8, exeU8, buildU8, versionU8;
    QString dir;                 // log dir (JSONL)
    bool dumpsReady = false;
    HealthEndpoint health;
    QString healthError;
    std::vector<unsigned char> stackBuf;
#ifdef _WIN32
    HANDLE mainThread = nullptr;
    DWORD64 stackBase = 0;
#endif

    struct Episode {
        qint64 start = 0, maxAge = 0;
        qint64 cpuRefT = 0, cpu0 = 0;   // main-thread CPU time sampled at (or just before) the start
        std::string activity;
        bool dumpTried = false;
        int worst = Stall;
    };
    // Main-thread CPU samples from the monitor's OK ticks (32 x poll >= 1.6 s, longer than stall + beat), so a stall's
    // CPU delta starts where the loop stopped, not where the monitor noticed.
    struct CpuSample { qint64 t = 0, cpu = 0; };
    CpuSample cpuRing[32];
    int cpuCount = 0, cpuHead = 0;

    qint64 lagAt(qint64 now) const { return std::max<qint64>(0, now - lastBeat.load() - o.beatMs); }

    std::string activityNow() const {
        std::string a = readActivity();
        if (a.empty() && !firstBeat.load()) a = "startup";
        return a;
    }

    qint64 recentMax(qint64 now) const {
        const qint64 b = now / 5000, id = bucketId.load();
        qint64 r = id == b ? std::max(curBucketMax.load(), prevBucketMax.load()) : id == b - 1 ? curBucketMax.load() : 0;
        return std::max(r, lagAt(now));
    }

    qint64 mainCpuMs() const {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if (!mainThread || !GetThreadTimes(mainThread, &c, &e, &k, &u)) return 0;
        auto v = [](const FILETIME& f) { return (qint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        return (v(k) + v(u)) / 10000;
#else
        return 0;
#endif
    }

    QString jsonlPath() const {
        return dir.isEmpty() ? QString() : QDir(dir).filePath(QStringLiteral("watchdog-%1-%2.jsonl")
                                                              .arg(QString::fromStdString(fileNameU8), QString::fromStdString(localDate())));
    }

    // Plain WriteFile, open-append-close per event (events are rare): never qDebug, never asynclog.
    void appendLine(const std::string& line) {
        if (dir.isEmpty()) return;
#ifdef _WIN32
        const std::wstring p = QDir::toNativeSeparators(jsonlPath()).toStdWString();
        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        const std::string l = line + "\n";
        DWORD w = 0;
        WriteFile(h, l.data(), DWORD(l.size()), &w, nullptr);
        CloseHandle(h);
#else
        FILE* f = std::fopen(jsonlPath().toLocal8Bit().constData(), "ab");
        if (f) { std::fwrite(line.data(), 1, line.size(), f); std::fputc('\n', f); std::fclose(f); }
#endif
    }

    void writeEvent(const char* event, const Episode* ep, qint64 lag, const std::function<void(Json&)>& extra = {}) {
        if (dir.isEmpty()) return;
        const qint64 now = steadyMs();
        Json j;
        j.str("ts", utcIsoNow()).str("event", event).str("name", nameU8).num("pid", qint64(QCoreApplication::applicationPid()))
         .num("lagMs", lag).str("activity", ep ? ep->activity : activityNow());
        if (ep) {   // busy (CPU rising) vs blocked (CPU flat): less than a quarter of the wall time on CPU = blocked
            const qint64 cpu = std::max<qint64>(0, mainCpuMs() - ep->cpu0), elapsed = std::max<qint64>(1, now - ep->cpuRefT);
            j.num("mainCpuDeltaMs", cpu).boolean("blocked", cpu * 4 < elapsed);
        }
        const ProcInfo pi = procInfo();
        j.tenths("privateMB", pi.privateMB).num("handles", pi.handles).num("threads", pi.threads);
        if (extra) extra(j);
        appendLine(j.done());
    }

    std::vector<std::string> sampleMainStack() {
#ifdef PP_WATCHDOG_STACKS
        if (!mainThread || stackBuf.empty()) return {};
        CONTEXT ctx{};
        size_t len = 0;
        const DWORD rc = captureRaw(mainThread, stackBase, &ctx, stackBuf.data(), stackBuf.size(), &len);
        if (rc != 0) return {std::string("(stack capture failed: ") + std::to_string(rc) + ")"};
        quint64 pcs[32];
        const int n = unwindCopy(&ctx, ctx.Rsp, stackBuf.data(), len, pcs, 32);
        return minidump::symbolize(pcs, n);
#else
        return {};
#endif
    }

    void doDump(Episode& ep, qint64 lag) {
        ep.dumpTried = true;
        QString err;
        const QString path = dumpsReady ? minidump::writeDump("hang", 20000, &err, &stop) : QString();
        if (!dumpsReady) err = QStringLiteral("dumps off (no log dir or no dbghelp)");
        if (!path.isEmpty()) dumps.fetch_add(1);
        writeEvent("dump", &ep, lag, [&](Json& j) {
            if (!path.isEmpty()) j.str("dump", utf8(QDir::toNativeSeparators(path)));
            else j.str("dumpError", utf8(err));
        });
    }

    void recover(Episode& ep) {
        const qint64 stalledMs = std::max(maxGapPending.exchange(0), ep.maxAge);
        const int worst = ep.worst;
        state.store(Ok);
        writeEvent("recovered", &ep, lagAt(steadyMs()), [&](Json& j) { j.num("stalledMs", stalledMs).str("worst", stateName(worst)); });
        const QString name = QString::fromStdString(nameU8), act = QString::fromStdString(ep.activity);
        const QString w = QString::fromLatin1(stateName(worst));
        LoopWatchdog* self = q;
        // Posting an event is thread-safe; the slot runs on the main thread, now that it runs again.
        QMetaObject::invokeMethod(q, [self, name, stalledMs, act, w]() {
            qWarning().noquote() << QStringLiteral("[Watchdog] %1: event loop %2 for %3 s%4").arg(name, w == "hang" ? "hung" : "stalled")
                                    .arg(stalledMs / 1000.0, 0, 'f', 1).arg(act.isEmpty() ? QString() : " in " + act);
            emit self->recovered(name, stalledMs, act, w);
        }, Qt::QueuedConnection);
    }

    void tick(Episode& ep) {
        const qint64 now = steadyMs();
        const qint64 age = now - lastBeat.load();
        const qint64 lag = std::max<qint64>(0, age - o.beatMs);
        if (exiting.load()) { state.store(Ok); return; }        // shutdown work is not a stall
        const qint64 cpuNow = mainCpuMs();
        if (state.load() == Ok && lag >= o.stallMs) {
            ep = Episode();
            ep.start = lastBeat.load() + o.beatMs;                  // when the beat was due
            ep.cpuRefT = now; ep.cpu0 = cpuNow;
            for (int i = 0; i < cpuCount; ++i) {                    // newest sample taken no later than the start
                const CpuSample& s = cpuRing[(cpuHead - 1 - i + 64) % 32];
                ep.cpuRefT = s.t; ep.cpu0 = s.cpu;
                if (s.t <= ep.start) break;
            }
            ep.activity = activityNow();
            ep.maxAge = age;
            maxGapPending.store(0);
            state.store(Stall);
            stalls.fetch_add(1);
            writeEvent("stall", &ep, lag);
        }
        if (state.load() == Ok) {
            cpuRing[cpuHead] = CpuSample{now, cpuNow};
            cpuHead = (cpuHead + 1) % 32;
            cpuCount = std::min(cpuCount + 1, 32);
            return;
        }
        ep.maxAge = std::max(ep.maxAge, age);
        if (lag < o.stallMs / 2) { recover(ep); return; }
        if (state.load() == Stall && lag >= o.hangMs) {
            state.store(Hang);
            ep.worst = Hang;
            hangs.fetch_add(1);
            const std::vector<std::string> stack = o.stackSample ? sampleMainStack() : std::vector<std::string>();
            writeEvent("hang", &ep, std::max<qint64>(lag, lagAt(steadyMs())), [&](Json& j) { if (!stack.empty()) j.strings("stack", stack); });
        }
        if (o.dumpAfterMs > 0 && !ep.dumpTried && lagAt(steadyMs()) >= o.dumpAfterMs) doDump(ep, lagAt(steadyMs()));
        if (o.abortAfterMs > 0 && lagAt(steadyMs()) >= o.abortAfterMs) {
            if (!ep.dumpTried) doDump(ep, lagAt(steadyMs()));
            writeEvent("abort", &ep, lagAt(steadyMs()), [&](Json& j) { j.num("abortAfterMs", o.abortAfterMs).str("exitCode", "0xDEAD"); });
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 0xDEAD);
#else
            std::_Exit(0xAD);
#endif
        }
    }

    void pruneOldJsonl() {
        if (dir.isEmpty() || o.keepDays <= 0) return;
        const QString prefix = QStringLiteral("watchdog-%1-").arg(QString::fromStdString(fileNameU8));
        const QDate cutoff = QDate::currentDate().addDays(-o.keepDays);
        for (const QFileInfo& fi : QDir(dir).entryInfoList({prefix + "*.jsonl"}, QDir::Files)) {
            const QString rest = fi.fileName().mid(prefix.size());
            if (rest.size() != 16) continue;                      // exactly yyyy-MM-dd.jsonl (not another name's file)
            const QDate d = QDate::fromString(rest.left(10), Qt::ISODate);
            if (d.isValid() && d < cutoff) QFile::remove(fi.absoluteFilePath());
        }
    }

    void run() {
        if (!dir.isEmpty()) { QDir().mkpath(dir); pruneOldJsonl(); }
        writeEvent("start", nullptr, 0, [&](Json& j) {
            j.str("exe", exeU8).str("build", buildU8).str("version", versionU8)
             .num("beatMs", o.beatMs).num("stallMs", o.stallMs).num("hangMs", o.hangMs).num("dumpAfterMs", o.dumpAfterMs)
             .num("abortAfterMs", o.abortAfterMs).num("healthPort", health.port()).boolean("dumps", dumpsReady);
            if (!healthError.isEmpty()) j.str("healthError", utf8(healthError));
        });
        const int pollMs = std::clamp(o.stallMs / 5, 20, 200);
        Episode ep;
        std::unique_lock<std::mutex> lk(m);
        while (!stop.load()) {
            cv.wait_for(lk, std::chrono::milliseconds(pollMs), [this]() { return stop.load(); });
            if (stop.load()) break;
            lk.unlock();
            tick(ep);
            lk.lock();
        }
    }

    // port 0 = any free port. The handler runs on the endpoint's thread and reads only atomics and immutable strings.
    bool startHealth(quint16 port, QString* error) {
        return health.start(port, [this](const std::string&, const std::string& path) {
            HealthEndpoint::Reply r;
            if (path == "/healthz" || path == "/health") r.body = healthBody(&r.status);
            else { r.status = 404; r.contentType = "text/plain"; r.body = "try /healthz\n"; }
            return r;
        }, error);
    }

    std::string healthBody(int* status) const {
        const qint64 now = steadyMs();
        const qint64 age = now - lastBeat.load(), lag = std::max<qint64>(0, age - o.beatMs);
        const char* st = exiting.load() ? "exiting" : lag >= o.hangMs ? "hang" : lag >= o.stallMs ? "stall" : "ok";
        // Always 200: the state is in the body. Clients drop the body of an error status (the app's stall banner,
        // SupervisorClient::probeHealth, does), and a hang is exactly when its activity and lag are wanted.
        if (status) *status = 200;
        Json j;
        j.str("name", nameU8).num("pid", qint64(QCoreApplication::applicationPid())).str("state", st).num("lagMs", lag)
         .str("activity", activityNow()).num("lastBeatAgeMs", age).num("uptimeS", (now - startMs) / 1000)
         .str("exe", exeU8).str("build", buildU8).str("version", versionU8)
         .num("stalls", qint64(stalls.load())).num("hangs", qint64(hangs.load())).num("dumps", qint64(dumps.load()))
         .num("maxLagMs", std::max(maxLag.load(), lag)).num("recentMaxLagMs", recentMax(now))
         .num("beatMs", o.beatMs).num("stallMs", o.stallMs).num("hangMs", o.hangMs);
        return j.done();
    }
};

// ---------------------------------------------------------------------------------------------------------------------

LoopWatchdog* LoopWatchdog::install(const LoopWatchdogOptions& o) {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return nullptr;
    delete s_instance;                                            // a second install replaces the first (tests)
    s_instance = new LoopWatchdog(o, app);
    return s_instance;
}

LoopWatchdog* LoopWatchdog::instance() { return s_instance; }

void LoopWatchdog::setActivity(const char* staticTag) {
    if (!onMainThread()) return;
    g_tag.store(staticTag, std::memory_order_release);
    g_textActive.store(false, std::memory_order_release);
}

void LoopWatchdog::setActivityText(const QByteArray& text) {
    if (!onMainThread()) return;
    writeText(text.constData(), size_t(text.size()));
}

QByteArray LoopWatchdog::activity() { return QByteArray::fromStdString(readActivity()); }

LoopWatchdog::LoopWatchdog(const LoopWatchdogOptions& opt, QObject* parent) : QObject(parent), d(new Impl) {
    d->q = this;
    d->o = opt;
    LoopWatchdogOptions& o = d->o;
    o.beatMs = std::max(10, o.beatMs);
    o.stallMs = std::max(o.beatMs / 2 + 1, o.stallMs);
    o.hangMs = std::max(o.stallMs, o.hangMs);
    d->startMs = steadyMs();
    d->lastBeat.store(d->startMs);
#ifdef _WIN32
    g_mainTid.store(GetCurrentThreadId());
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &d->mainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    d->stackBase = DWORD64(reinterpret_cast<quintptr>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase));
#endif
    d->nameU8 = utf8(o.name.isEmpty() ? QCoreApplication::applicationName() : o.name);
    d->fileNameU8 = utf8(minidump::sanitizedName(QString::fromStdString(d->nameU8)));
    const QString exe = QCoreApplication::applicationFilePath();
    d->exeU8 = utf8(QDir::toNativeSeparators(exe));
    d->buildU8 = utf8(QFileInfo(exe).lastModified().toUTC().toString(Qt::ISODate));
    d->versionU8 = utf8(QCoreApplication::applicationVersion());
    d->dir = o.logDir;
    if (!o.logDir.isEmpty()) {
        minidump::Options mo;
        mo.dir = QDir(o.logDir).filePath("dumps");
        mo.name = QString::fromStdString(d->nameU8);
        mo.maxPerHour = o.maxDumpsPerHour;
        mo.keep = o.keepDumps;
        d->dumpsReady = minidump::init(mo);
        if (d->dumpsReady && o.crashDumps) minidump::installCrashHandler();
    }
    if (o.stackSample) d->stackBuf.resize(256 * 1024);

    d->timer.setTimerType(Qt::PreciseTimer);
    d->timer.setInterval(o.beatMs);
    connect(&d->timer, &QTimer::timeout, this, [this]() { beat(); });
    d->timer.start();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() { d->exiting.store(true); d->timer.stop(); });

    if (o.healthPort != 0) d->startHealth(o.healthPort < 0 ? 0 : quint16(std::min(o.healthPort, 65535)), &d->healthError);
    d->monitor = std::thread([impl = d.get()]() { impl->run(); });

    const QString health = d->health.isRunning() ? QStringLiteral("/healthz on 127.0.0.1:%1").arg(d->health.port())
                         : o.healthPort != 0 ? QStringLiteral("/healthz NOT started (%1)").arg(d->healthError) : QStringLiteral("no /healthz");
    const QString line = QStringLiteral("[Watchdog] %1: beat %2 ms, stall %3 ms, hang %4 ms, dump %5, abort %6; %7; %8")
        .arg(QString::fromStdString(d->nameU8)).arg(o.beatMs).arg(o.stallMs).arg(o.hangMs)
        .arg(o.dumpAfterMs > 0 && d->dumpsReady ? QStringLiteral("%1 ms").arg(o.dumpAfterMs) : QStringLiteral("off"))
        .arg(o.abortAfterMs > 0 ? QStringLiteral("%1 ms").arg(o.abortAfterMs) : QStringLiteral("off"), health,
             d->dir.isEmpty() ? QStringLiteral("no files") : QDir::toNativeSeparators(QDir(d->dir).filePath(
                 QStringLiteral("watchdog-%1-YYYY-MM-DD.jsonl").arg(QString::fromStdString(d->fileNameU8)))));
    if (o.healthPort != 0 && !d->health.isRunning()) qWarning().noquote() << line;
    else qInfo().noquote() << line;
}

LoopWatchdog::~LoopWatchdog() {
    d->stop.store(true);
    d->cv.notify_all();
    if (d->monitor.joinable()) d->monitor.join();
    d->health.stop();
    d->timer.stop();
#ifdef _WIN32
    if (d->mainThread) CloseHandle(d->mainThread);
#endif
    if (s_instance == this) s_instance = nullptr;
}

void LoopWatchdog::beat() {
    const qint64 now = steadyMs();
    const qint64 prev = d->lastBeat.exchange(now);
    const qint64 gap = now - prev;
    const qint64 lag = std::max<qint64>(0, gap - d->o.beatMs);
    const qint64 b = now / 5000, id = d->bucketId.load(std::memory_order_relaxed);
    if (b != id) {                                                // 5 s buckets: recentMaxLagMs covers 5-10 s
        d->prevBucketMax.store(b == id + 1 ? d->curBucketMax.load() : 0);
        d->curBucketMax.store(0);
        d->bucketId.store(b);
    }
    if (lag > d->curBucketMax.load(std::memory_order_relaxed)) d->curBucketMax.store(lag);
    if (lag > d->maxLag.load(std::memory_order_relaxed)) d->maxLag.store(lag);
    qint64 g = d->maxGapPending.load(std::memory_order_relaxed);
    while (gap > g && !d->maxGapPending.compare_exchange_weak(g, gap)) {}
    if (!d->firstBeat.load(std::memory_order_relaxed)) d->firstBeat.store(true);
}

LoopWatchdog::Stats LoopWatchdog::stats() const {
    Stats s;
    const qint64 now = steadyMs();
    s.state = d->state.load();
    s.lagMs = d->lagAt(now);
    s.recentMaxLagMs = d->recentMax(now);
    s.maxLagMs = std::max(d->maxLag.load(), s.lagMs);                 // includes a stall still in progress
    s.stalls = d->stalls.load();
    s.hangs = d->hangs.load();
    s.dumps = d->dumps.load();
    s.healthPort = d->health.port();
    return s;
}

bool LoopWatchdog::setHealthPort(quint16 port, QString* error) {
    if (port != 0 && d->health.isRunning() && d->health.port() == port) return true;
    d->health.stop();
    if (port == 0) return true;                                   // off
    return d->startHealth(port, error);
}

quint16 LoopWatchdog::healthPort() const { return d->health.port(); }
const LoopWatchdogOptions& LoopWatchdog::options() const { return d->o; }
QString LoopWatchdog::jsonlPath() const { return d->jsonlPath(); }
QByteArray LoopWatchdog::healthJson() const { return QByteArray::fromStdString(d->healthBody(nullptr)); }

// ---------------------------------------------------------------------------------------------------------------------

void WatchdogScope::save() {
    m_active = true;
    m_prevTag = g_tag.load(std::memory_order_relaxed);
    m_prevText = g_textActive.load(std::memory_order_relaxed);
    if (m_prevText) std::memcpy(m_prevBuf, g_text, sizeof(m_prevBuf));   // only the main thread writes it
}

WatchdogScope::WatchdogScope(const char* staticTag) {
    if (!onMainThread()) return;
    save();
    LoopWatchdog::setActivity(staticTag);
}

WatchdogScope::WatchdogScope(const QByteArray& text) {
    if (!onMainThread()) return;
    save();
    LoopWatchdog::setActivityText(text);
}

WatchdogScope::~WatchdogScope() {
    if (!m_active) return;
    g_tag.store(m_prevTag, std::memory_order_release);
    if (m_prevText) { m_prevBuf[sizeof(m_prevBuf) - 1] = 0; writeText(m_prevBuf, std::strlen(m_prevBuf)); }
    else g_textActive.store(false, std::memory_order_release);
}
