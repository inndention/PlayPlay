#include "blockingworker.h"
#include <cstdio>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QMetaObject>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

struct BlockingWorker::Shared {
    Options opt;
    std::mutex mu;
    std::condition_variable cv;          // worker waits for a job or quit
    std::condition_variable exitedCv;    // destructor waits for the thread to leave its loop
    struct Job { quint64 id = 0; QByteArray key; Work work; Token token; };
    std::deque<Job> queue;
    quint64 nextId = 1;
    BlockingWorker* owner = nullptr;     // cleared by the destructor; the thread posts results only while set
    bool quit = false, exited = false, started = false;
    // running job (guarded by mu)
    bool running = false;
    quint64 runningId = 0;
    QByteArray runningKey;
    Token runningToken;
    QElapsedTimer runningClock;
    qint64 lastWarnMs = -1;              // runningClock ms of the last "still blocked" line (-1: none yet)
    // counters (guarded by mu)
    quint64 done = 0, cancelled = 0, coalesced = 0, refused = 0;
    qint64 lastMs = 0, maxMs = 0;

    static void loop(std::shared_ptr<Shared> s);
};

BlockingWorker::BlockingWorker(const QString& name, QObject* parent) : BlockingWorker([&] { Options o; o.name = name; return o; }(), parent) {}

BlockingWorker::BlockingWorker(const Options& o, QObject* parent) : QObject(parent), d(std::make_shared<Shared>()) {
    d->opt = o;
    if (d->opt.name.isEmpty()) d->opt.name = "worker";
    d->opt.warnMs = qMax(0, d->opt.warnMs);
    d->opt.detachMs = qMax(0, d->opt.detachMs);
    d->opt.maxQueue = qMax(1, d->opt.maxQueue);
    d->owner = this;
}

BlockingWorker::~BlockingWorker() {
    std::shared_ptr<Shared> s = d;
    bool wasStarted = false, stuck = false;
    QByteArray stuckKey; qint64 stuckMs = 0;
    {
        std::unique_lock<std::mutex> l(s->mu);
        s->owner = nullptr;                                    // no result may be posted to us from here on
        s->quit = true;
        s->cancelled += quint64(s->queue.size());
        s->queue.clear();
        if (s->runningToken) s->runningToken->store(true);
        wasStarted = s->started;
        s->cv.notify_all();
        if (wasStarted && !s->exited) {
            s->exitedCv.wait_for(l, std::chrono::milliseconds(s->opt.detachMs), [&] { return s->exited; });
            if (!s->exited) { stuck = true; stuckKey = s->runningKey; stuckMs = s->running ? s->runningClock.elapsed() : 0; }
        }
    }
    if (stuck)
        qWarning().noquote() << "[Worker" << s->opt.name << "] job" << QString::fromLatin1(stuckKey) << "still blocked after" << stuckMs << "ms; leaving its thread";
    // The thread (if any) was detached at start(); it holds its own reference to Shared and exits on quit.
}

void BlockingWorker::start() {
    // Called with d->mu held. One thread per worker, detached: the Shared block is what it may touch after we are gone.
    if (d->started) return;
    d->started = true;
    std::thread(&Shared::loop, d).detach();
}

quint64 BlockingWorker::post(const QByteArray& key, Work work) {
    if (!work) return 0;
    bool warnRunning = false; qint64 runningMs = 0; QByteArray runningKey;
    quint64 id = 0;
    {
        std::lock_guard<std::mutex> l(d->mu);
        if (d->quit) return 0;
        if (!key.isEmpty()) {
            for (auto it = d->queue.begin(); it != d->queue.end(); ++it) {
                if (it->key != key) continue;
                // Replace in place: the job keeps its slot (FIFO order) and gets the newer closure.
                it->work = std::move(work);
                it->token = std::make_shared<std::atomic<bool>>(false);
                it->id = d->nextId++;
                ++d->coalesced;
                id = it->id;
                break;
            }
        }
        if (!id) {
            if (int(d->queue.size()) >= d->opt.maxQueue) { ++d->refused; return 0; }
            Shared::Job j; j.id = d->nextId++; j.key = key; j.work = std::move(work); j.token = std::make_shared<std::atomic<bool>>(false);
            id = j.id;
            d->queue.push_back(std::move(j));
        }
        if (d->running && d->opt.warnMs > 0) {
            runningMs = d->runningClock.elapsed();
            if (runningMs >= d->opt.warnMs && (d->lastWarnMs < 0 || runningMs - d->lastWarnMs >= 60000)) { warnRunning = true; runningKey = d->runningKey; d->lastWarnMs = runningMs; }
        }
        start();
        d->cv.notify_one();
    }
    if (warnRunning)
        qWarning().noquote() << "[Worker" << d->opt.name << "] job" << QString::fromLatin1(runningKey) << "still running after" << runningMs << "ms;" << "queue" << stats().queued;
    return id;
}

bool BlockingWorker::cancel(quint64 id) {
    if (!id) return false;
    std::lock_guard<std::mutex> l(d->mu);
    for (auto it = d->queue.begin(); it != d->queue.end(); ++it)
        if (it->id == id) { d->queue.erase(it); ++d->cancelled; return true; }
    if (d->running && d->runningId == id) {
        if (!d->runningToken->exchange(true)) ++d->cancelled;
        return true;
    }
    return false;
}

void BlockingWorker::cancelAll() {
    std::lock_guard<std::mutex> l(d->mu);
    d->cancelled += quint64(d->queue.size());
    d->queue.clear();
    if (d->running && d->runningToken && !d->runningToken->exchange(true)) ++d->cancelled;
}

bool BlockingWorker::isBusy(const QByteArray& key) const {
    std::lock_guard<std::mutex> l(d->mu);
    if (d->running && d->runningKey == key && !d->runningToken->load()) return true;
    for (const auto& j : d->queue) if (j.key == key) return true;
    return false;
}

bool BlockingWorker::isIdle() const {
    std::lock_guard<std::mutex> l(d->mu);
    return !d->running && d->queue.empty();
}

BlockingWorker::Stats BlockingWorker::stats() const {
    Stats s;
    std::lock_guard<std::mutex> l(d->mu);
    s.queued = int(d->queue.size());
    s.running = d->running;
    s.runningKey = d->runningKey;
    s.runningMs = d->running ? d->runningClock.elapsed() : 0;
    s.done = d->done; s.cancelled = d->cancelled; s.coalesced = d->coalesced; s.refused = d->refused;
    s.lastMs = d->lastMs; s.maxMs = d->maxMs;
    return s;
}

QJsonObject BlockingWorker::statsJson() const {
    const Stats s = stats();
    QJsonObject o;
    o["name"] = d->opt.name;
    o["queued"] = s.queued;
    o["running"] = s.running;
    o["runningKey"] = QString::fromLatin1(s.runningKey);
    o["runningMs"] = double(s.runningMs);
    o["done"] = double(s.done); o["cancelled"] = double(s.cancelled); o["coalesced"] = double(s.coalesced); o["refused"] = double(s.refused);
    o["lastMs"] = double(s.lastMs); o["maxMs"] = double(s.maxMs);
    return o;
}

QString BlockingWorker::name() const { return d->opt.name; }
const BlockingWorker::Options& BlockingWorker::options() const { return d->opt; }

static void warn(bool detached, const QByteArray& msg) {
    if (!detached) { qWarning().noquote() << QString::fromUtf8(msg); return; }
    const QByteArray line = msg + '\n';
    fwrite(line.constData(), 1, size_t(line.size()), stderr);
    fflush(stderr);
}

void BlockingWorker::Shared::loop(std::shared_ptr<Shared> s) {
#ifdef Q_OS_WIN
    {
        const QString n = "pp-worker-" + s->opt.name;
        SetThreadDescription(GetCurrentThread(), reinterpret_cast<const wchar_t*>(n.utf16()));   // visible in dumps / debuggers
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);                      // never competes with the loop
    }
#endif
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> l(s->mu);
            s->cv.wait(l, [&] { return s->quit || !s->queue.empty(); });
            if (s->quit) break;
            job = std::move(s->queue.front());
            s->queue.pop_front();
            s->running = true; s->runningId = job.id; s->runningKey = job.key; s->runningToken = job.token;
            s->runningClock.start(); s->lastWarnMs = -1;
        }
        Done done;
        QByteArray threw;
        try { done = job.work(job.token); }
        catch (const std::exception& e) { threw = QByteArray("threw: ") + e.what(); done = Done(); }
        catch (...) { threw = "threw"; done = Done(); }
        qint64 ms = 0; bool deliver = false;
        {
            std::lock_guard<std::mutex> l(s->mu);
            ms = s->runningClock.elapsed();
            s->running = false; s->runningId = 0; s->runningKey.clear(); s->runningToken.reset();
            s->lastMs = ms; if (ms > s->maxMs) s->maxMs = ms;
            const bool cancelled = job.token->load();
            if (!cancelled) ++s->done;
            deliver = !cancelled && done && s->owner && !s->quit;
            // Post while holding the lock: the destructor clears `owner` under the same lock, so a post never races
            // with the object going away (QObject's destructor then drops the queued call).
            if (deliver) QMetaObject::invokeMethod(s->owner, [done = std::move(done)]() { done(); }, Qt::QueuedConnection);
        }
        // A detached thread (owner gone, quit set) may finish a hung job during static teardown after main() returned:
        // Qt's message handler / asynclog may be gone by then, so such a thread only writes plain stderr.
        bool detached = false;
        { std::lock_guard<std::mutex> l(s->mu); detached = s->quit || !s->owner; }
        if (!threw.isEmpty())
            warn(detached, "[Worker " + s->opt.name.toUtf8() + "] job " + job.key + " " + threw);
        if (s->opt.warnMs > 0 && ms >= s->opt.warnMs)
            warn(detached, "[Worker " + s->opt.name.toUtf8() + "] job " + job.key + " took " + QByteArray::number(ms) + " ms");
    }
    {
        std::lock_guard<std::mutex> l(s->mu);
        s->exited = true;
        s->exitedCv.notify_all();
    }
}
