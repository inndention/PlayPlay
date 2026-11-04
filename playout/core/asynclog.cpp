#include "asynclog.h"
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace asynclog {
namespace {

qint64 steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void nameThread(const wchar_t* name, bool belowNormal) {
#ifdef Q_OS_WIN
    // SetThreadDescription is Windows 10 1607+; looked up so the binary still loads anywhere. Names show in dumps.
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
        if (auto fn = reinterpret_cast<SetDesc>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription")))) fn(GetCurrentThread(), name);
    if (belowNormal) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    Q_UNUSED(name); Q_UNUSED(belowNormal);
#endif
}

struct Entry {
    qint64 utcMs = 0;
    int type = QtDebugMsg;
    quint64 dropped = 0;    // lines lost to a full ring just before this one
    QByteArray cat;         // empty for the "default" category
    QString msg;
};

// Bounded MPSC ring: Vyukov's queue with a sequence number per cell. Producers (any thread) claim a cell with one CAS
// on the head and publish it with a release store; nothing blocks, and a full ring fails the push at once. One
// consumer at a time (the caller serialises consumers: State::fileMutex for the file ring, the echo thread alone for
// the echo ring). A producer preempted between claim and publish only delays the consumer at that cell.
class Ring {
public:
    explicit Ring(int capacity) {
        size_t cap = 2;
        while (cap < size_t(qMax(2, capacity))) cap <<= 1;
        m_mask = cap - 1;
        m_cells.reset(new Cell[cap]);
        for (size_t i = 0; i < cap; ++i) m_cells[i].seq.store(i, std::memory_order_relaxed);
    }
    template <typename Fill> bool tryPush(Fill&& fill) {
        size_t pos = m_head.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = m_cells[pos & m_mask];
            const size_t seq = c.seq.load(std::memory_order_acquire);
            const std::ptrdiff_t dif = std::ptrdiff_t(seq) - std::ptrdiff_t(pos);
            if (dif == 0) {
                if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    fill(c.value);
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;                                           // full
            } else {
                pos = m_head.load(std::memory_order_relaxed);           // another producer took it
            }
        }
    }
    bool tryPop(Entry& out) {
        const size_t pos = m_tail.load(std::memory_order_relaxed);
        Cell& c = m_cells[pos & m_mask];
        if (std::ptrdiff_t(c.seq.load(std::memory_order_acquire)) - std::ptrdiff_t(pos + 1) < 0) return false;
        out = std::move(c.value);
        c.value = Entry();                                              // Qt's move swaps: drop the old payload here
        c.seq.store(pos + m_mask + 1, std::memory_order_release);
        m_tail.store(pos + 1, std::memory_order_release);
        return true;
    }
    bool hasReady() const {
        const size_t pos = m_tail.load(std::memory_order_acquire);
        return std::ptrdiff_t(m_cells[pos & m_mask].seq.load(std::memory_order_acquire)) - std::ptrdiff_t(pos + 1) >= 0;
    }
    size_t headPos() const { return m_head.load(std::memory_order_acquire); }
    size_t tailPos() const { return m_tail.load(std::memory_order_acquire); }

private:
    struct Cell { std::atomic<size_t> seq{0}; Entry value; };
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    size_t m_mask = 0;
    std::unique_ptr<Cell[]> m_cells;
};

// Sleep / wake handshake for one consumer thread. The mutex only covers the consumer's decision to sleep (never I/O),
// so a producer that has to wake it waits at most for that check. The seq_cst fences pair up (Dekker): either the
// consumer sees the new cell, or the producer sees `sleeping` and notifies under the mutex, so no wake-up is lost.
struct Waker {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> sleeping{false};
    void wake() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_relaxed)) { std::lock_guard<std::mutex> g(m); cv.notify_one(); }
    }
    template <typename Ready> void sleep(Ready ready, int ms) {
        std::unique_lock<std::mutex> l(m);
        sleeping.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!ready()) cv.wait_for(l, std::chrono::milliseconds(ms));
        sleeping.store(false, std::memory_order_relaxed);
    }
};

constexpr qint64 kNoDateOverride = LLONG_MIN;
std::atomic<qint64> g_dateOverrideJd{kNoDateOverride};
std::atomic<bool> g_writerPaused{false};
QtMessageHandler g_previous = nullptr;
// True on the logger's own threads (and inside its locked sections): a Qt message raised there is dropped instead of
// re-entering a lock this thread already holds.
thread_local bool t_inLogger = false;

struct InLogger {
    bool was;
    InLogger() : was(t_inLogger) { t_inLogger = true; }
    ~InLogger() { t_inLogger = was; }
};

QDate currentDay() {
    const qint64 jd = g_dateOverrideJd.load(std::memory_order_relaxed);
    return jd != kNoDateOverride ? QDate::fromJulianDay(jd) : QDate::currentDate();
}

QByteArray formatLine(const Entry& e) {
    static const char* levels[] = {"debug", "warning", "critical", "fatal", "info"};
    QJsonObject o;
    o["ts"] = QDateTime::fromMSecsSinceEpoch(e.utcMs, QTimeZone(QTimeZone::UTC)).toString(Qt::ISODateWithMs);
    o["level"] = levels[e.type >= 0 && e.type < 5 ? e.type : 0];
    o["msg"] = e.msg;
    if (!e.cat.isEmpty()) o["cat"] = QString::fromUtf8(e.cat);
    if (e.dropped) o["dropped"] = qint64(e.dropped);
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

class State {
public:
    explicit State(const Options& o) : opt(o), ring(o.ringSize > 0 ? o.ringSize : 8192),
        echoRing(o.consoleRingSize > 0 ? o.consoleRingSize : 1024) {
        if (opt.flushMs <= 0) opt.flushMs = 250;
    }

    void start() {
        accepting.store(true, std::memory_order_release);
        writer = std::thread([this] { writerLoop(); });
        if (opt.consoleEcho) echo = std::thread([this] { echoLoop(); });
    }

    // Hot path (any thread): never blocks; at most a CAS, a release store and, if the writer sleeps, one notify.
    void push(Entry&& e) {
        const bool ok = ring.tryPush([&](Entry& cell) {
            cell = std::move(e);
            if (droppedPending.load(std::memory_order_relaxed)) cell.dropped = droppedPending.exchange(0);
        });
        if (ok) { writerWake.wake(); return; }
        droppedPending.fetch_add(1, std::memory_order_relaxed);
        droppedTotal.fetch_add(1, std::memory_order_relaxed);
    }

    // qFatal, and every message once the sink has stopped: written on the calling thread with a bounded lock wait.
    void writeDirect(const Entry& e, bool fatal) {
        InLogger guard;
        if (fileStuck.load()) { if (!fatal) { writeErrors.fetch_add(1); return; } }
        std::unique_lock<std::timed_mutex> l(fileMutex, std::defer_lock);
        if (l.try_lock_for(std::chrono::milliseconds(fatal ? 2000 : 500))) {
            drainLocked(0);                                     // everything logged before this line goes first
            rotateIfNeeded();
            if (file.isOpen() && file.write(formatLine(e)) >= 0) written.fetch_add(1); else writeErrors.fetch_add(1);
            if (file.isOpen()) file.flush();
            dirty = false;
            publishFlushed(ring.tailPos());
            return;
        }
        if (fatal) {
            // The writer holds the file (disk stalled?): append the fatal line through a second handle.
            QFile f(currentPath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { f.write(formatLine(e)); f.close(); return; }
        }
        writeErrors.fetch_add(1);
    }

    bool flush(int timeoutMs) {
        if (!accepting.load()) return !fileStuck.load();       // stopped: lines are written synchronously
        const size_t target = ring.headPos();
        flushWaiters.fetch_add(1);
        writerWake.wake();
        std::unique_lock<std::mutex> l(doneM);
        const bool ok = doneCv.wait_for(l, std::chrono::milliseconds(qMax(0, timeoutMs)),
                                        [&] { return flushedPos.load() >= target; });
        flushWaiters.fetch_sub(1);
        return ok;
    }

    void stop() {
        if (stopped.exchange(true)) return;
        InLogger guard;
        accepting.store(false, std::memory_order_release);     // later messages take writeDirect
        stopWriter.store(true, std::memory_order_release);
        writerWake.wake();
        bool joined = false;
        {
            std::unique_lock<std::mutex> l(doneM);
            joined = doneCv.wait_for(l, std::chrono::milliseconds(3000), [&] { return writerDone.load(); });
        }
        if (joined) {
            writer.join();
            // A producer that saw accepting=true just before the flip may land after the writer's last pass.
            std::lock_guard<std::timed_mutex> fl(fileMutex);
            drainLocked(0);
            if (dirty && file.isOpen()) file.flush();
            dirty = false;
        } else {
            writer.detach();                                    // disk stalled: never hang process exit on it
            fileStuck.store(true);
        }
        if (echo.joinable()) {
            stopEcho.store(true, std::memory_order_release);
            echoWake.wake();
            bool echoJoined = false;
            {
                std::unique_lock<std::mutex> l(doneM);
                echoJoined = doneCv.wait_for(l, std::chrono::milliseconds(300), [&] { return echoDone.load(); });
            }
            if (echoJoined) echo.join(); else echo.detach();    // blocked console write: leave it behind
        }
    }

    void closeFile() {
        std::unique_lock<std::timed_mutex> l(fileMutex, std::defer_lock);
        if (!l.try_lock_for(std::chrono::milliseconds(500))) return;
        if (file.isOpen()) { file.flush(); file.close(); }
    }

    QString currentPath() { std::lock_guard<std::mutex> g(pathMutex); return path; }

    Stats stats() const {
        Stats s;
        s.written = written.load(); s.dropped = droppedTotal.load();
        s.consoleWritten = echoWritten.load(); s.consoleDropped = echoDropped.load();
        s.writeErrors = writeErrors.load(); s.installed = true; s.consoleEcho = opt.consoleEcho;
        return s;
    }

    Options opt;
    std::atomic<bool> accepting{false};

private:
    struct Drain { int lines = 0; bool urgent = false; };

    void writerLoop() {
        nameThread(L"PlayPlay log writer", false);
        t_inLogger = true;
        qint64 lastFlush = steadyMs();
        for (;;) {
            const bool stopping = stopWriter.load(std::memory_order_acquire);
            bool wrote = false, empty = true;
            if (stopping || !g_writerPaused.load(std::memory_order_relaxed)) {
                std::lock_guard<std::timed_mutex> l(fileMutex);
                const Drain d = drainLocked(stopping ? 0 : 2048);
                wrote = d.lines > 0;
                empty = !ring.hasReady();
                const qint64 now = steadyMs();
                if (dirty && (empty || d.urgent || flushWaiters.load() > 0 || now - lastFlush >= opt.flushMs)) {
                    if (file.isOpen()) file.flush();
                    dirty = false;
                    lastFlush = now;
                }
                if (!dirty) publishFlushed(ring.tailPos());
            } else {
                empty = !ring.hasReady();
            }
            if (stopping && empty) break;
            if (wrote && !empty) continue;
            const bool paused = g_writerPaused.load(std::memory_order_relaxed);
            writerWake.sleep([&] {
                return stopWriter.load(std::memory_order_acquire) || (!g_writerPaused.load(std::memory_order_relaxed) && ring.hasReady());
            }, paused ? 5 : opt.flushMs);
        }
        {
            std::lock_guard<std::timed_mutex> l(fileMutex);
            if (dirty && file.isOpen()) file.flush();
            dirty = false;
            publishFlushed(ring.tailPos());
        }
        { std::lock_guard<std::mutex> g(doneM); writerDone.store(true); }
        doneCv.notify_all();
    }

    void echoLoop() {
        nameThread(L"PlayPlay log echo", true);
        t_inLogger = true;
        Entry e;
        for (;;) {
            if (echoRing.tryPop(e)) { writeConsole(e); echoWritten.fetch_add(1, std::memory_order_relaxed); continue; }
            if (stopEcho.load(std::memory_order_acquire)) break;
            echoWake.sleep([&] { return stopEcho.load(std::memory_order_acquire) || echoRing.hasReady(); }, 250);
        }
        { std::lock_guard<std::mutex> g(doneM); echoDone.store(true); }
        doneCv.notify_all();
    }

    void writeConsole(const Entry& e) {
        if (opt.echoViaPreviousHandler && g_previous) {
            const QMessageLogContext ctx(nullptr, 0, nullptr, e.cat.isEmpty() ? "default" : e.cat.constData());
            g_previous(QtMsgType(e.type), ctx, e.msg);
            return;
        }
#ifdef Q_OS_WIN
        // Raw handle writes, not fprintf: a write blocked here holds no CRT stream lock that exit() would wait for.
        const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (!h || h == INVALID_HANDLE_VALUE) return;
        DWORD mode = 0, n = 0;
        if (GetConsoleMode(h, &mode)) {
            const QString line = e.msg + QLatin1Char('\n');
            WriteConsoleW(h, reinterpret_cast<const wchar_t*>(line.utf16()), DWORD(line.size()), &n, nullptr);
        } else {
            const QByteArray b = e.msg.toUtf8() + '\n';
            WriteFile(h, b.constData(), DWORD(b.size()), &n, nullptr);
        }
#else
        const QByteArray b = e.msg.toUtf8() + '\n';
        const ssize_t r = ::write(2, b.constData(), size_t(b.size()));
        Q_UNUSED(r);
#endif
    }

    // Consumer side of `ring`; the caller holds fileMutex. limit 0 = until empty.
    Drain drainLocked(int limit) {
        Drain d;
        if (!ring.hasReady()) return d;
        rotateIfNeeded();
        QByteArray buf;
        Entry e;
        while ((limit <= 0 || d.lines < limit) && ring.tryPop(e)) {
            buf += formatLine(e);
            ++d.lines;
            if (e.type == QtWarningMsg || e.type == QtCriticalMsg) d.urgent = true;
            if (opt.consoleEcho) {
                if (echoRing.tryPush([&](Entry& cell) { cell = e; })) echoWake.wake();
                else echoDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (file.isOpen() && file.write(buf) >= 0) { written.fetch_add(quint64(d.lines)); dirty = true; }
        else writeErrors.fetch_add(quint64(d.lines));
        return d;
    }

    // Daily file, opened on the writer thread; a failed open is retried at most once a second. Caller holds fileMutex.
    void rotateIfNeeded() {
        const QDate today = currentDay();
        if (file.isOpen() && today == fileDate) return;
        const qint64 now = steadyMs();
        if (!file.isOpen() && today == fileDate && now - lastOpenTryMs < 1000) return;
        lastOpenTryMs = now;
        if (file.isOpen()) { file.flush(); file.close(); dirty = false; }
        fileDate = today;
        QDir().mkpath(opt.dir);
        const QString p = QDir(opt.dir).filePath(opt.baseName + "-" + today.toString(Qt::ISODate) + ".log");
        file.setFileName(p);
        file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        { std::lock_guard<std::mutex> g(pathMutex); path = p; }
        const QDate cutoff = today.addDays(-opt.keepDays);
        for (const QFileInfo& fi : QDir(opt.dir).entryInfoList({opt.baseName + "-*.log"}, QDir::Files)) {
            const QDate d = QDate::fromString(fi.completeBaseName().mid(opt.baseName.size() + 1), Qt::ISODate);
            if (d.isValid() && d < cutoff) QFile::remove(fi.filePath());
        }
    }

    // seq_cst store / load against flush()'s waiter count: either the waiter sees the new position or we see it waiting.
    void publishFlushed(size_t pos) {
        flushedPos.store(pos);
        if (flushWaiters.load() == 0) return;
        { std::lock_guard<std::mutex> g(doneM); }
        doneCv.notify_all();
    }

    Ring ring, echoRing;
    Waker writerWake, echoWake;
    std::timed_mutex fileMutex;         // the file, and the consumer side of `ring`
    QFile file;
    QDate fileDate;
    qint64 lastOpenTryMs = 0;
    bool dirty = false;                 // bytes handed to QFile since the last flush
    std::mutex pathMutex;
    QString path;
    std::atomic<bool> stopWriter{false}, stopEcho{false}, writerDone{false}, echoDone{false}, stopped{false}, fileStuck{false};
    std::atomic<quint64> droppedPending{0}, droppedTotal{0}, written{0}, echoWritten{0}, echoDropped{0}, writeErrors{0};
    std::atomic<size_t> flushedPos{0};
    std::atomic<int> flushWaiters{0};
    std::mutex doneM;
    std::condition_variable doneCv;
    std::thread writer, echo;
};

std::atomic<State*> g_state{nullptr};
std::mutex g_installMutex;              // install / shutdown only, never on the logging path
bool g_handlerInstalled = false;
bool g_atexitRegistered = false;

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) { if (g_previous) g_previous(type, ctx, msg); return; }
    if (t_inLogger) return;                                     // raised inside the logger itself
    Entry e;
    e.utcMs = QDateTime::currentMSecsSinceEpoch();
    e.type = int(type);
    if (ctx.category && std::strcmp(ctx.category, "default") != 0) e.cat = QByteArray(ctx.category);
    e.msg = msg;
    if (type == QtFatalMsg) { s->writeDirect(e, true); return; }   // Qt aborts when we return
    if (!s->accepting.load(std::memory_order_acquire)) { s->writeDirect(e, false); return; }
    s->push(std::move(e));
}

void atexitShutdown() { shutdown(false); }

} // namespace

void applyEnvironment(Options& o) {
    if (qEnvironmentVariableIntValue("PLAYPLAY_CONSOLE_LOG") || qEnvironmentVariableIntValue("PLAYPLAY_LOG_STDERR")) o.consoleEcho = true;
    if (qEnvironmentVariableIntValue("PLAYPLAY_LOG_DEBUGGER")) { o.consoleEcho = true; o.echoViaPreviousHandler = true; }
}

void install(const Options& o) {
    std::lock_guard<std::mutex> g(g_installMutex);
    State* old = g_state.load();
    if (old) old->stop();
    // Old states are never deleted: a handler call on another thread may still hold the pointer. Re-installing only
    // happens in tests.
    State* s = new State(o);
    s->start();
    g_state.store(s, std::memory_order_release);
    if (old) old->closeFile();
    if (!g_handlerInstalled) { g_previous = qInstallMessageHandler(&handler); g_handlerInstalled = true; }
    if (!g_atexitRegistered) { std::atexit(&atexitShutdown); g_atexitRegistered = true; }
}

bool flush(int timeoutMs) {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? s->flush(timeoutMs) : true;
}

void shutdown(bool restorePreviousHandler) {
    std::lock_guard<std::mutex> g(g_installMutex);
    State* s = g_state.load();
    if (!s) return;
    s->stop();
    if (restorePreviousHandler && g_handlerInstalled) {
        qInstallMessageHandler(g_previous);
        g_handlerInstalled = false;
        g_state.store(nullptr, std::memory_order_release);
        s->closeFile();
    }
}

QString currentPath() {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? s->currentPath() : QString();
}

Stats stats() {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? s->stats() : Stats();
}

namespace testing {
void setDateOverride(const QDate& d) { g_dateOverrideJd.store(d.isValid() ? d.toJulianDay() : kNoDateOverride); }
void setWriterPaused(bool paused) { g_writerPaused.store(paused); }
QtMessageHandler handler() { return &asynclog::handler; }
}

} // namespace asynclog
