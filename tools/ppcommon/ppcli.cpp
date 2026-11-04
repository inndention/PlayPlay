#include "ppcli.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#else
#  include <cerrno>
#  include <unistd.h>
#endif

namespace ppcli {

namespace {
std::mutex& errMutex() { static std::mutex m; return m; }
std::mutex& outMutex() { static std::mutex m; return m; }
qint64 nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void writeErr(const QByteArray& b) { std::lock_guard<std::mutex> g(errMutex()); std::fwrite(b.constData(), 1, size_t(b.size()), stderr); std::fflush(stderr); }
// Every stdout line goes through here so a --serve reply and a one-shot result never interleave.
bool writeOut(const QByteArray& b) {
    std::lock_guard<std::mutex> g(outMutex());
    const size_t n = std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    return n == size_t(b.size()) && std::fflush(stdout) == 0;
}
// The Supervisor's PROGRESS regex takes [\d.]+ only: no sign, no exponent, no nan.
QString plain(double v, int decimals) { if (!std::isfinite(v) || v < 0) v = 0; return QString::number(v, 'f', std::clamp(decimals, 0, 6)); }
QString rstrip(QString s) { while (!s.isEmpty() && s.back().isSpace()) s.chop(1); return s; }
bool looksNumeric(const QString& a) { bool ok = false; a.toDouble(&ok); return ok; }
}   // namespace

// ----------------------------------------------------------------------------- one-shot contract
void initStdio() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);           // "\n" stays "\n": JSON-lines readers split on it
#endif
    // A child's stdout/stderr are pipes, where Qt on Windows would send qWarning to the debugger instead.
    if (!qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) qputenv("QT_FORCE_STDERR_LOGGING", "1");
}

void log(const QString& msg) { writeErr(rstrip(msg).toUtf8() + '\n'); }
QByteArray progressLine(double done, double total, int decimals) { return ("PROGRESS " + plain(done, decimals) + ' ' + plain(total, decimals)).toUtf8() + '\n'; }
void progress(double done, double total, int decimals) { writeErr(progressLine(done, total, decimals)); }

QByteArray resultLine(const QJsonObject& r) { return QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n'; }
int exitCodeFor(const QJsonObject& r) { return r.value("ok").toBool() ? ExitOk : (r.value("unsupported").toBool() ? ExitUnsupported : ExitFailed); }
int finish(const QJsonObject& r) { writeOut(resultLine(r)); return exitCodeFor(r); }
QJsonObject failure(const QString& error, const QJsonObject& extra) { QJsonObject o = extra; o["ok"] = false; o["error"] = error; return o; }
QJsonObject unsupportedResult(const QString& what, const QJsonObject& extra) { QJsonObject o = failure("unsupported: " + what, extra); o["unsupported"] = true; return o; }
int fail(const QString& error, const QJsonObject& extra) { return finish(failure(error, extra)); }
int unsupported(const QString& what, const QJsonObject& extra) { return finish(unsupportedResult(what, extra)); }

QJsonObject parseResult(const QByteArray& stdoutBytes, QString* err) {
    const QList<QByteArray> lines = stdoutBytes.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray t = it->trimmed();
        if (t.isEmpty()) continue;
        QJsonParseError pe; const QJsonDocument d = QJsonDocument::fromJson(t, &pe);
        if (pe.error == QJsonParseError::NoError && d.isObject()) return d.object();
        if (err) *err = "last stdout line is not a JSON object: " + QString::fromUtf8(t.left(200));
        return {};
    }
    if (err) *err = "no output";
    return {};
}

bool parseProgress(const QByteArray& line, double* done, double* total) {
    const QList<QByteArray> p = line.simplified().split(' ');
    if (p.size() != 3 || p[0] != "PROGRESS") return false;
    bool a = false, b = false; const double d = p[1].toDouble(&a), t = p[2].toDouble(&b);
    if (!a || !b) return false;
    if (done) *done = d;
    if (total) *total = t;
    return true;
}

bool isUnsupported(int exitCode, const QJsonObject& r) { return exitCode == ExitUnsupported || r.value("unsupported").toBool(); }

// ----------------------------------------------------------------------------- Args
Args::Args(const QStringList& args, const QStringList& spec, bool allowPositional) {
    QHash<QString, bool> takesValue;
    for (QString s : spec) { const bool v = s.endsWith('='); if (v) s.chop(1); takesValue.insert(s, v); }
    bool optionsDone = false;
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (!optionsDone && a == "--") { optionsDone = true; continue; }
        if (!optionsDone && a.size() > 1 && a.startsWith('-') && !looksNumeric(a)) {
            QString name = a, val; bool inlineValue = false;
            const int eq = a.indexOf('=');
            if (a.startsWith("--") && eq > 2) { name = a.left(eq); val = a.mid(eq + 1); inlineValue = true; }
            const auto it = takesValue.constFind(name);
            if (it == takesValue.cend()) { m_unknown = name; m_error = "unsupported: " + name; m_unsupported = true; return; }
            if (!it.value()) {
                if (inlineValue) { m_error = name + " takes no value"; return; }
                m_values[name];                                   // a flag: present, no values
                continue;
            }
            if (!inlineValue) {
                if (i + 1 >= args.size()) { m_error = name + " needs a value"; return; }
                val = args[++i];
            }
            m_values[name].append(val);
            continue;
        }
        if (!allowPositional) { m_unknown = a; m_error = "unsupported: " + a; m_unsupported = true; return; }
        m_positional << a;
    }
}

QString Args::value(const QString& name, const QString& def) const { const QStringList v = m_values.value(name); return v.isEmpty() ? def : v.last(); }
double Args::number(const QString& name, double def, bool* ok) const {
    if (ok) *ok = true;
    if (!has(name)) return def;
    bool good = false; const double v = value(name).toDouble(&good);
    if (ok) *ok = good && std::isfinite(v);
    return good && std::isfinite(v) ? v : def;
}
int Args::integer(const QString& name, int def, bool* ok) const {
    if (ok) *ok = true;
    if (!has(name)) return def;
    bool good = false; const int v = value(name).toInt(&good);
    if (ok) *ok = good;
    return good ? v : def;
}
int Args::report() const { return m_unsupported ? unsupported(m_unknown) : fail(m_error.isEmpty() ? QString("bad arguments") : m_error); }

// ----------------------------------------------------------------------------- --serve
struct ServeJob {
    QJsonValue id;
    QString op;
    QJsonObject req;
    int timeoutMs = 0;
    std::atomic<bool> cancelled{false};
    bool timedOut = false;                    // guarded by ServeState::outM (decides whether anything more is written)
    qint64 startMs = -1;                      // guarded by ServeState::m
    qint64 hangDeadline = 0;                  // main thread only
    QJsonObject result;                       // guarded by ServeState::m
};

struct ServeState {
    std::mutex m;
    std::condition_variable cv;               // wakes the main loop
    std::condition_variable workCv;           // wakes workers
    std::deque<QByteArray> inbox;
    bool eof = false, readerDone = false, stopping = false;
    quint64 starts = 0;                       // bumped when a worker starts a job: the main loop arms its deadline
    std::deque<std::shared_ptr<ServeJob>> queue;
    std::vector<std::shared_ptr<ServeJob>> finished;

    std::mutex outM;
    std::function<bool(const QByteArray&)> write;
    std::atomic<bool> outBroken{false};

    bool writeLocked(const QJsonObject& o) {  // outM held
        if (outBroken) return false;
        if (!write(resultLine(o))) { outBroken = true; return false; }
        return true;
    }
    bool send(const QJsonObject& o) { std::lock_guard<std::mutex> g(outM); return writeLocked(o); }
    // Writes `o` unless the job already got its timeout reply; `close` makes this the job's last line.
    bool sendFor(ServeJob& j, const QJsonObject& o, bool close) {
        std::lock_guard<std::mutex> g(outM);
        if (j.timedOut) return false;
        if (close) j.timedOut = true;
        return writeLocked(o);
    }

    static QJsonObject reply(const QJsonValue& id, QJsonObject r) {
        if (!r.contains("ok")) r["ok"] = !r.contains("error");
        r["id"] = id;
        return r;
    }

    static void work(std::shared_ptr<ServeState> st, ServeLoop::Handler h) {
        for (;;) {
            std::shared_ptr<ServeJob> j;
            {
                std::unique_lock<std::mutex> lk(st->m);
                st->workCv.wait(lk, [&] { return st->stopping || !st->queue.empty(); });
                if (st->queue.empty()) return;
                j = st->queue.front(); st->queue.pop_front();
                j->startMs = nowMs();
                ++st->starts;
            }
            st->cv.notify_all();                  // the main loop arms this job's deadline
            QJsonObject r;
            try { ServeContext ctx(j, st); r = h(j->req, ctx); }
            catch (const std::exception& e) { r = failure(QStringLiteral("exception: ") + QString::fromLocal8Bit(e.what())); }
            catch (...) { r = failure(QStringLiteral("exception")); }
            { std::lock_guard<std::mutex> g(st->m); j->result = r; st->finished.push_back(j); }
            st->cv.notify_all();
        }
    }
};

QJsonValue ServeContext::id() const { return m_job->id; }
QString ServeContext::op() const { return m_job->op; }
bool ServeContext::cancelled() const { return m_job->cancelled.load(); }
const std::atomic<bool>* ServeContext::cancelFlag() const { return &m_job->cancelled; }   // the job outlives the handler call
qint64 ServeContext::elapsedMs() const { return nowMs() - m_job->startMs; }   // startMs was set on this thread before the handler ran
void ServeContext::event(const QJsonObject& ev) {
    if (m_job->cancelled.load()) return;
    QJsonObject o = ev;
    if (!o.contains("event")) o["event"] = "event";
    o["id"] = m_job->id;
    m_state->sendFor(*m_job, o, false);
}
void ServeContext::progress(double done, double total) { QJsonObject o; o["event"] = "progress"; o["done"] = done; o["total"] = total; event(o); }

namespace {
// stdin through ReadFile/read, not the CRT: a reader blocked in fgets would hold the stdin lock that the CRT's exit
// path takes, so the process could never exit while waiting for a request. CancelSynchronousIo unblocks it.
class StdinReader {
public:
    explicit StdinReader(int maxLine) : m_maxLine(maxLine) {}
    ~StdinReader() {
#ifdef _WIN32
        if (m_thread) CloseHandle(m_thread);
#endif
    }
    bool readLine(QByteArray& out) {
        for (;;) {
            const int nl = m_buf.indexOf('\n');
            if (nl >= 0) {
                const bool skipped = m_skipping;
                out = m_buf.left(nl); m_buf.remove(0, nl + 1); m_skipping = false;
                if (skipped) continue;                         // tail of an over-long line already reported
                if (out.endsWith('\r')) out.chop(1);
                return true;
            }
            if (m_buf.size() > m_maxLine) {                    // no newline in sight: report it once, drop the rest
                if (m_skipping) { m_buf.clear(); }
                else { out = m_buf.left(m_maxLine + 1); m_buf.clear(); m_skipping = true; return true; }
            }
            if (m_eof) {
                if (m_buf.isEmpty() || m_skipping) return false;
                out = m_buf; m_buf.clear();                    // last line without a newline
                if (out.endsWith('\r')) out.chop(1);
                return true;
            }
            fill();
        }
    }
    void cancel() {
        std::lock_guard<std::mutex> g(m_m);
        m_cancelled = true;
#ifdef _WIN32
        if (m_thread) CancelSynchronousIo(m_thread);
#endif
    }
private:
    void fill() {
        {
            std::lock_guard<std::mutex> g(m_m);
            if (m_cancelled) { m_eof = true; return; }
#ifdef _WIN32
            if (!m_thread) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
#endif
        }
        m_chunk.resize(64 * 1024);
#ifdef _WIN32
        DWORD got = 0;
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (!in || in == INVALID_HANDLE_VALUE || !ReadFile(in, m_chunk.data(), DWORD(m_chunk.size()), &got, nullptr) || got == 0) { m_eof = true; return; }
        m_buf.append(m_chunk.constData(), int(got));
#else
        for (;;) {
            const ssize_t got = ::read(0, m_chunk.data(), size_t(m_chunk.size()));
            if (got < 0 && errno == EINTR) { std::lock_guard<std::mutex> g(m_m); if (m_cancelled) { m_eof = true; return; } continue; }
            if (got <= 0) { m_eof = true; return; }
            m_buf.append(m_chunk.constData(), int(got));
            return;
        }
#endif
    }
    const int m_maxLine;
    QByteArray m_buf, m_chunk;
    bool m_eof = false, m_skipping = false;
    std::mutex m_m;
    bool m_cancelled = false;
#ifdef _WIN32
    HANDLE m_thread = nullptr;
#endif
};

void stopReader(const std::shared_ptr<ServeState>& st, std::thread& reader, const std::function<void()>& cancelRead) {
    const qint64 until = nowMs() + 500;
    std::unique_lock<std::mutex> lk(st->m);
    while (!st->readerDone && nowMs() < until) {
        lk.unlock();
        if (cancelRead) cancelRead();                     // retried: a cancel can land just before the read starts
        lk.lock();
        st->cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return st->readerDone; });
    }
    const bool done = st->readerDone;
    lk.unlock();
    if (done) reader.join(); else reader.detach();        // a read that ignores cancellation dies with the process
}
}   // namespace

ServeLoop::ServeLoop() = default;
ServeLoop::ServeLoop(Options o) : m_opt(std::move(o)) {}

QJsonObject ServeLoop::unsupportedOp(const QString& op) { return unsupportedResult("op " + (op.isEmpty() ? QStringLiteral("(none)") : op)); }

int ServeLoop::run(const Handler& handler) {
    Options o = m_opt;
    o.workers = std::clamp(o.workers, 1, 64);
    o.defaultTimeoutMs = std::max(1, o.defaultTimeoutMs);
    o.maxTimeoutMs = std::max(o.defaultTimeoutMs, o.maxTimeoutMs);
    o.hangGraceMs = std::max(0, o.hangGraceMs);
    o.maxPending = std::max(1, o.maxPending);
    o.maxLineBytes = std::max(1024, o.maxLineBytes);
    if (!o.readLine) {
        auto in = std::make_shared<StdinReader>(o.maxLineBytes);
        o.readLine = [in](QByteArray& l) { return in->readLine(l); };
        o.cancelRead = [in] { in->cancel(); };
    }
    if (!o.writeLine) { initStdio(); o.writeLine = writeOut; }

    auto st = std::make_shared<ServeState>();
    st->write = o.writeLine;
    std::thread reader([st, rd = o.readLine] {
        QByteArray line;
        for (;;) {
            line.clear();
            const bool got = rd(line);
            std::lock_guard<std::mutex> g(st->m);
            if (!got) { st->eof = true; st->readerDone = true; st->cv.notify_all(); return; }
            st->inbox.push_back(line);
            st->cv.notify_all();
        }
    });
    std::vector<std::thread> workers;
    for (int i = 0; i < o.workers; ++i) workers.emplace_back(&ServeState::work, st, handler);

    std::vector<std::shared_ptr<ServeJob>> inflight;      // queued or running; main thread only
    bool quitting = false, eofSeen = false, hung = false;
    quint64 seenStarts = 0;
    qint64 lastActivity = nowMs();
    int exitCode = -1;
    while (exitCode < 0) {
        std::deque<QByteArray> lines;
        std::vector<std::shared_ptr<ServeJob>> done, due;
        qint64 now = 0;
        {
            std::unique_lock<std::mutex> lk(st->m);
            now = nowMs();
            qint64 wake = now + 1000;
            for (const auto& j : inflight) {
                if (j->hangDeadline > 0) wake = std::min(wake, j->hangDeadline);
                else if (j->startMs >= 0) wake = std::min(wake, j->startMs + j->timeoutMs);
            }
            if (o.idleExitMs > 0 && inflight.empty()) wake = std::min(wake, lastActivity + o.idleExitMs);
            st->cv.wait_for(lk, std::chrono::milliseconds(std::max<qint64>(1, wake - now)),
                            [&] { return !st->inbox.empty() || !st->finished.empty() || (st->eof && !eofSeen) || st->starts != seenStarts; });
            lines.swap(st->inbox);
            done.swap(st->finished);
            eofSeen = st->eof;
            seenStarts = st->starts;
            now = nowMs();
            for (const auto& j : inflight)
                if (j->hangDeadline == 0 && j->startMs >= 0 && now >= j->startMs + j->timeoutMs) due.push_back(j);
        }

        for (const auto& j : done) {
            inflight.erase(std::remove(inflight.begin(), inflight.end(), j), inflight.end());
            if (j->hangDeadline > 0) log(QString("serve: %1 finished after its timeout; result dropped").arg(j->op));
            else st->sendFor(*j, ServeState::reply(j->id, j->result), true);
            lastActivity = now;
        }
        for (const auto& j : due) {
            if (std::find(inflight.begin(), inflight.end(), j) == inflight.end()) continue;   // finished meanwhile
            j->cancelled = true;
            j->hangDeadline = now + o.hangGraceMs;
            st->sendFor(*j, ServeState::reply(j->id, failure(QString("timeout after %1 ms").arg(j->timeoutMs), QJsonObject{{"timeout", true}})), true);
            log(QString("serve: %1 timed out after %2 ms").arg(j->op).arg(j->timeoutMs));
        }

        for (QByteArray& line : lines) {
            lastActivity = now;
            const QByteArray t = line.trimmed();
            if (t.isEmpty()) continue;
            if (t.size() > o.maxLineBytes) { st->send(ServeState::reply(QJsonValue::Null, failure("request too large"))); continue; }
            QJsonParseError pe; const QJsonDocument d = QJsonDocument::fromJson(t, &pe);
            if (pe.error != QJsonParseError::NoError || !d.isObject()) {
                st->send(ServeState::reply(QJsonValue::Null, failure("bad request: " + (pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("not a JSON object")))));
                continue;
            }
            const QJsonObject req = d.object();
            const QJsonValue id = req.contains("id") ? req.value("id") : QJsonValue(QJsonValue::Null);
            const QString op = req.value("op").toString();
            if (op == "ping") { st->send(ServeState::reply(id, QJsonObject{{"ok", true}, {"pong", true}, {"pid", double(QCoreApplication::applicationPid())}, {"busy", int(inflight.size())}})); continue; }
            if (op == "quit" || op == "shutdown") { quitting = true; st->send(ServeState::reply(id, QJsonObject{{"ok", true}, {"bye", true}})); continue; }
            if (quitting) { st->send(ServeState::reply(id, failure("shutting down"))); continue; }
            if (int(inflight.size()) >= o.maxPending) { st->send(ServeState::reply(id, failure("busy", QJsonObject{{"busy", true}}))); continue; }
            auto j = std::make_shared<ServeJob>();
            j->id = id; j->op = op; j->req = req;
            const int want = req.value("timeoutMs").toInt(0);
            j->timeoutMs = want > 0 ? std::min(want, o.maxTimeoutMs) : o.defaultTimeoutMs;
            inflight.push_back(j);
            { std::lock_guard<std::mutex> g(st->m); st->queue.push_back(j); }
            st->workCv.notify_one();
        }

        for (const auto& j : inflight) if (j->hangDeadline > 0 && now >= j->hangDeadline) hung = true;
        if (hung) break;
        if (st->outBroken && !quitting) {                 // nobody reads the replies any more: stop taking work
            log("serve: stdout closed; finishing in-flight requests and exiting");
            quitting = true;
            for (const auto& j : inflight) j->cancelled = true;
        }
        if ((eofSeen || quitting) && inflight.empty()) exitCode = st->outBroken ? int(ExitFailed) : int(ExitOk);
        else if (o.idleExitMs > 0 && inflight.empty() && now - lastActivity >= o.idleExitMs) {
            log(QString("serve: idle for %1 s; exiting").arg(o.idleExitMs / 1000.0, 0, 'f', 1));
            exitCode = ExitOk;
        }
    }

    if (hung) {
        log(QString("serve: a request is still running %1 ms after its timeout; exiting so the client restarts the tool").arg(o.hangGraceMs));
        std::deque<std::shared_ptr<ServeJob>> queued;
        { std::lock_guard<std::mutex> g(st->m); queued.swap(st->queue); st->stopping = true; }
        st->workCv.notify_all();
        for (const auto& j : inflight) {
            j->cancelled = true;
            st->sendFor(*j, ServeState::reply(j->id, failure("server restarting (another request hung)", QJsonObject{{"restart", true}})), true);
        }
        for (auto& w : workers) w.detach();               // they own the shared state; the stuck one cannot be joined
        stopReader(st, reader, o.cancelRead);
        if (o.hardExitOnHang) std::_Exit(ExitFailed);    // every line is already flushed; skip destructors a stuck thread may block
        return ExitFailed;
    }

    { std::lock_guard<std::mutex> g(st->m); st->stopping = true; }
    st->workCv.notify_all();
    for (auto& w : workers) w.join();                    // all idle: nothing in flight
    stopReader(st, reader, o.cancelRead);
    return exitCode;
}

}   // namespace ppcli
