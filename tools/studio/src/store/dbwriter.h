#pragma once
// The single writer of studio.db (ST-03, docs/plans/starmedia-studio.md §4.8).
//
// Qt SQL connections are bound to the thread that opened them, and SQLite allows one writer at a time anyway, so every
// write of the Studio goes through one DbWriter thread with one connection:
//
//   - Batches. Writes queue up and are committed together, one transaction per ~50 ms (batchWindowMs, counted from the
//     oldest queued write) or per maxBatchOps writes, whichever comes first. Thousands of job / event rows per second
//     then cost a few fsyncs instead of one each.
//   - Commit before send. A Durable write (money rows: spend reserve / charge / release; provider-accepted markers: a
//     provider job id that must be known before the first poll) wakes the writer at once, and its batch is committed
//     with synchronous=FULL (the WAL is fsynced). Its callback runs only after COMMIT returned, so a caller that sends
//     the paid request from that callback (commitThenSend) can never have sent a request whose money row is not on
//     disk — the pub lane's "persist `publishing` BEFORE any network call". If the commit fails, the callback reports
//     the error and nothing is sent: fail closed.
//   - Isolation. Each write runs inside its own SAVEPOINT. A write that fails (a constraint, a bad statement, an
//     exception, Tx::fail) is rolled back alone; the other writes of the batch still commit. A failing BEGIN or COMMIT
//     fails every write of that batch (nothing of it is in the database).
//   - Order. Writes execute in submission order. Callbacks are delivered after the commit, in submission order, on the
//     thread of the `context` object given to submit() (through a per-thread dispatcher, and only while the context
//     still exists), or on the writer thread when no context is given.
//   - Never blocks the caller. submit() only queues (bounded by maxQueue; a full queue refuses with false). flush()
//     and stop() block and are for shutdown and tests.
//
// Crash safety comes from SQLite: a process killed in the middle of a batch leaves the transaction uncommitted, and
// the next open rolls it back from the WAL; every write whose callback reported success is in the database.
#include <QObject>
#include <QPointer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QThread>
#include <QVariant>
#include <QVariantList>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace studio {
class Clock;
}

namespace studio::store {

namespace detail {
struct WriterEngine;                       // the writer thread's connection and statement cache (dbwriter.cpp)
}

enum class Durability {
    Batched,                               // committed within ~batchWindowMs, synchronous=NORMAL
    Durable,                               // committed now, synchronous=FULL (money rows, provider-accepted markers)
};

/// What a write's callback receives.
struct WriteResult {
    bool ok = false;
    QString error;                         // set when !ok
    QVariant value;                        // whatever the write set with Tx::setValue (an event seq, a row id ...)
    qint64 lastInsertId = -1;              // rowid of the write's last successful INSERT, -1 if none
    quint64 batch = 0;                     // 1, 2, ... (0 when the write never reached a batch)
    bool durable = false;                  // its batch was committed with synchronous=FULL
    qint64 committedMs = 0;                // wall clock (the writer's Clock) right after COMMIT
};

/// The handle a write gets on the writer thread. Valid only during the write.
class Tx {
public:
    /// Prepares (statements are cached by text), binds `binds` positionally (QVariant() = NULL; one per placeholder, a
    /// count mismatch fails) and executes one statement. False on error, which also marks the write failed (its
    /// savepoint is rolled back). Transaction control (BEGIN, COMMIT, ROLLBACK, END, SAVEPOINT, RELEASE), PRAGMA, ATTACH,
    /// DETACH and VACUUM are refused, also behind leading comments.
    bool exec(const QString& sql, const QVariantList& binds = {});
    /// A SELECT inside the write (read-modify-write), prepared afresh each time (not cached). Returns the active query
    /// positioned before the first row; on error an inactive query, and the write is marked failed.
    QSqlQuery select(const QString& sql, const QVariantList& binds = {});
    qint64 lastInsertId() const { return m_lastInsertId; }
    int rowsAffected() const { return m_rowsAffected; }

    /// Fails the write (its changes are rolled back); the first reason wins.
    void fail(const QString& reason);
    bool failed() const { return m_failed; }
    const QString& error() const { return m_error; }
    /// Passed to the callback as WriteResult::value.
    void setValue(const QVariant& v) { m_value = v; }
    /// The writer's clock (stamps for rows written here).
    qint64 nowMs() const;
    /// The writer's connection, for what exec()/select() cannot express. Never start or end a transaction on it.
    QSqlDatabase db() const;

private:
    friend class DbWriter;
    explicit Tx(detail::WriterEngine* e) : m_engine(e) {}
    detail::WriterEngine* m_engine;
    bool m_failed = false;
    QString m_error;
    QVariant m_value;
    qint64 m_lastInsertId = -1;
    int m_rowsAffected = 0;
};

using WriteFn = std::function<void(Tx&)>;

/// One unit of work: everything `fn` does commits or rolls back together. Builders in repos.h set the durability their
/// rows need (spend rows and provider-accepted markers are always Durable); submit() never lowers it.
struct Write {
    WriteFn fn;
    Durability durability = Durability::Batched;
    QString tag;                           // for logs and stats ("spend.append", "jobs.insert")
};

/// Several writes as one atomic unit (one savepoint), in order; durable if any part is. The first failing part fails
/// the whole unit.
Write combine(QList<Write> parts, const QString& tag = {});

class DbWriter {
public:
    struct Options {
        QString path;                      // studio.db (already migrated: Db::open runs the migrations first)
        int batchWindowMs = 50;
        int maxBatchOps = 5000;
        int maxQueue = 200000;             // queued writes; more are refused (submit() returns false)
        int busyTimeoutMs = 5000;          // waiting for another connection's lock (sqlite3 shell, a second process)
        int slowBatchMs = 500;             // a batch taking longer is logged as a warning
        const Clock* clock = nullptr;      // Tx::nowMs and WriteResult::committedMs; nullptr = Clock::system()
        /// Tests only: runs on the writer thread after each write of a batch (inside the open transaction), with the
        /// batch number and the write's index in it. Used to kill the process mid-batch.
        std::function<void(quint64 batch, int index)> afterWriteHook;
    };
    struct Stats {
        quint64 submitted = 0, committed = 0, failed = 0, refused = 0;
        quint64 batches = 0, durableBatches = 0, failedBatches = 0;
        int queued = 0;
        int lastBatchWrites = 0;
        qint64 lastBatchMs = 0, maxBatchMs = 0;
    };
    using Callback = std::function<void(const WriteResult&)>;

    explicit DbWriter(Options o);
    ~DbWriter();                           // stop(); a writer stuck past its deadline is waited for, never freed under it
    DbWriter(const DbWriter&) = delete;
    DbWriter& operator=(const DbWriter&) = delete;

    /// Starts the thread and opens its connection (WAL, busy timeout, synchronous=NORMAL). Blocks until the connection
    /// is open or failed: call it at start-up, before the event loop serves requests.
    bool start(QString* error = nullptr);
    bool running() const;

    /// Queues `w`. `cb` (optional) runs after the commit (or the failure) on `context`'s thread (which needs a running
    /// event loop), skipped when the context was destroyed meanwhile; without a context it runs on the writer thread
    /// and must be quick (it may submit(), but never flush() or stop(): that would wait for itself). False when the
    /// writer is not running, `w.fn` is empty or the queue is full: then `cb` never runs.
    bool submit(Write w, QObject* context = nullptr, Callback cb = {});
    bool submit(WriteFn fn, Durability d, QObject* context = nullptr, Callback cb = {}) {
        return submit(Write{std::move(fn), d, {}}, context, std::move(cb));
    }
    /// Commit before send: commits `w` durably (whatever its own durability) and only then calls `send` on
    /// `context`'s thread; on failure calls `failed` (if given) instead, and `send` never runs. `context` is required
    /// (a network request belongs to its thread, never to the writer thread). False when refused: neither runs.
    bool commitThenSend(Write w, QObject* context, Callback send, Callback failed = {});

    /// Blocks until every write submitted before the call is committed or failed (callbacks may still be in flight to
    /// their threads). Shutdown and tests only. False on timeout or when not running.
    bool flush(int timeoutMs = 10000);
    /// Commits what is queued, checkpoints the WAL, closes the connection and joins the thread. Later submits are
    /// refused. A negative `timeoutMs` waits without a deadline. False when the writer is still busy at the deadline:
    /// its thread is left running and keeps using this object, so the owner must not destroy it: leak it (Db::close
    /// does) or call stop() again. The destructor waits for a stuck writer rather than free memory it still uses.
    bool stop(int timeoutMs = 30000);

    Stats stats() const;
    QString path() const { return m_opt.path; }

private:
    struct Item {
        Write w;
        QPointer<QObject> context;
        QObject* dispatcher = nullptr;     // per-thread relay living in the context's thread; nullptr = writer thread
        Callback cb;
        quint64 seq = 0;
        qint64 enqueuedSteadyMs = 0;
    };
    void threadMain();
    void runBatch(std::deque<Item>& items);
    void deliver(Item& item, const WriteResult& r);
    QObject* dispatcherFor(QThread* t);

    Options m_opt;
    const Clock* m_clock;
    QThread* m_thread = nullptr;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;        // writer: work, flush request or stop
    std::condition_variable m_progress;    // flush(): a batch finished
    std::deque<Item> m_queue;
    int m_durableQueued = 0;
    bool m_accepting = false;
    bool m_stopping = false;
    bool m_threadDone = false;
    quint64 m_nextSeq = 0;                 // last seq handed out
    quint64 m_doneSeq = 0;                 // every seq <= this one is finished
    quint64 m_flushTarget = 0;             // commit at once up to this seq
    Stats m_stats;

    struct Dispatcher { QPointer<QThread> thread; QObject* obj; };
    std::mutex m_dispMutex;
    std::unordered_map<QThread*, Dispatcher> m_dispatchers;

    QString m_connName;
    std::unique_ptr<detail::WriterEngine> m_engine;   // writer-thread state (connection, statement cache)
    QString m_startError;
    bool m_started = false;
};

} // namespace studio::store
