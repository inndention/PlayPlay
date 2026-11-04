#ifndef PLAYPLAY_BLOCKINGWORKER_H
#define PLAYPLAY_BLOCKINGWORKER_H

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

/**
 * BlockingWorker — one named serial queue on a thread of its own for calls that may block, with the result applied
 * back on the owner's thread (docs/plans/perf-supervisor-app.md rank 11 / W14, slice PS-8).
 *
 * Why: the Supervisor has one event loop and it drives air. File stats on a network or removable volume, a recursive
 * walk of a watch folder and QStorageInfo::mountedVolumes() can each block for seconds (an unreachable UNC path: ~21 s
 * on this box) and did so on the loop every 3-5 s. A worker takes the blocking part; the loop keeps the state.
 *
 * Shape: post(key, work). `work` runs on the worker thread with a cancel token and returns a `Done` closure, which the
 * worker posts back to its owner's thread (a queued call on this QObject) unless the job was cancelled. Writing it as
 * "compute on the worker, return what to apply on the loop" keeps every QObject, store and member touch on the loop:
 * the worker thread only ever sees the plain values the caller copied into `work`.
 *
 *   - Serial: one job at a time, FIFO. A worker that is blocked in a syscall blocks only its own queue, so callers
 *     with independent concerns (sweep, walk, drives) use one worker each.
 *   - Coalescing: a non-empty key replaces a job that is still queued under the same key, so a timer that fires
 *     faster than the work finishes leaves one job waiting instead of a pile. The running job is never replaced.
 *   - Cancel: a queued job is dropped; a running one has its token set and its Done is not delivered.
 *   - Destruction: everything is cancelled; the thread gets `detachMs` to come back from a running job, then it is
 *     detached (a thread stuck in a network stat cannot be joined; leaving it is what lets the process exit) and
 *     cannot touch this object again: the shared state outlives both, and posting back needs the owner pointer
 *     that the destructor cleared under the lock. A Done already posted but not run dies with the QObject.
 *   - Diagnostics: stats() / statsJson() (queued, running key and ms, done, cancelled, coalesced, refused, last and
 *     max ms); a job over warnMs logs one warning when it ends, and a job still running after warnMs logs once a
 *     minute when the next post sees it.
 *
 * Cost on the owner's thread: a mutex lock per post and one queued call per result.
 */
class BlockingWorker : public QObject {
    Q_OBJECT
public:
    using Token = std::shared_ptr<std::atomic<bool>>;   // true once the job is cancelled; work may poll it
    using Done = std::function<void()>;                 // applied on the owner's thread; empty = nothing to apply
    using Work = std::function<Done(const Token&)>;     // runs on the worker thread

    struct Options {
        QString name;           // thread name and log prefix
        int warnMs = 2000;      // a job over this logs a warning
        int detachMs = 2000;    // destructor waits this long for a running job before leaving the thread
        int maxQueue = 64;      // post() refuses beyond this many queued jobs (returns 0)
    };
    struct Stats {
        int queued = 0;
        bool running = false;
        QByteArray runningKey;
        qint64 runningMs = 0;
        quint64 done = 0, cancelled = 0, coalesced = 0, refused = 0;
        qint64 lastMs = 0, maxMs = 0;
    };

    explicit BlockingWorker(const QString& name, QObject* parent = nullptr);
    explicit BlockingWorker(const Options& o, QObject* parent = nullptr);
    ~BlockingWorker() override;

    /// Queues `work`; returns its job id (0 = refused, queue full). A non-empty key replaces a queued job with the
    /// same key (see coalescing above).
    quint64 post(const QByteArray& key, Work work);

    /// Typed form: `work` computes an R on the worker thread, `done(R)` applies it on the owner's thread.
    template <class R>
    quint64 post(const QByteArray& key, std::function<R(const Token&)> work, std::function<void(R)> done) {
        return post(key, [work = std::move(work), done = std::move(done)](const Token& t) -> Done {
            R r = work(t);
            if (t->load()) return Done();
            auto box = std::make_shared<R>(std::move(r));
            return [done, box]() { done(std::move(*box)); };
        });
    }

    /// Queued: dropped (true). Running: token set, its Done is not delivered (true). Unknown / finished: false.
    bool cancel(quint64 id);
    void cancelAll();
    /// True while a job with this key is queued or running.
    bool isBusy(const QByteArray& key) const;
    bool isIdle() const;

    Stats stats() const;
    QJsonObject statsJson() const;
    QString name() const;
    const Options& options() const;

private:
    struct Shared;
    std::shared_ptr<Shared> d;
    void start();
};

#endif // PLAYPLAY_BLOCKINGWORKER_H
