#include "store/dbwriter.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QSqlError>
#include <atomic>
#include <chrono>
#include <exception>

#include "core/clock.h"
#include "core/log.h"
#include "store/db.h"

namespace studio::store {
namespace {

constexpr auto kCat = u"store";
constexpr int kMaxCachedStatements = 256;

qint64 steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QString queryError(const QSqlQuery& q) {
    const QSqlError e = q.lastError();
    QString t = e.databaseText().trimmed();
    if (t.isEmpty()) t = e.text().trimmed();
    return t.isEmpty() ? QStringLiteral("unknown SQL error") : t;
}

// The statement's first keyword, upper-cased, after leading whitespace and comments: SQLite skips those, so
// "/**/COMMIT" and "-- x\nCOMMIT" are still a COMMIT. An unterminated block comment runs to the end, as in SQLite.
QString leadingKeyword(const QString& sql) {
    const qsizetype n = sql.size();
    qsizetype i = 0;
    for (;;) {
        while (i < n && sql[i].isSpace()) ++i;
        if (i + 1 < n && sql[i] == u'-' && sql[i + 1] == u'-') {
            while (i < n && sql[i] != u'\n') ++i;
        } else if (i + 1 < n && sql[i] == u'/' && sql[i + 1] == u'*') {
            const qsizetype end = sql.indexOf(u"*/", i + 2);
            i = end < 0 ? n : end + 2;
        } else {
            break;
        }
    }
    qsizetype j = i;
    while (j < n && sql[j].isLetter()) ++j;
    return sql.mid(i, j - i).toUpper();
}

// Writes must not end the batch's transaction or change connection state behind the writer's back.
bool forbiddenInWrite(const QString& kw) {
    static const QStringList banned = {
        QStringLiteral("BEGIN"), QStringLiteral("COMMIT"), QStringLiteral("END"), QStringLiteral("ROLLBACK"),
        QStringLiteral("SAVEPOINT"), QStringLiteral("RELEASE"), QStringLiteral("PRAGMA"), QStringLiteral("ATTACH"),
        QStringLiteral("DETACH"), QStringLiteral("VACUUM")};
    return banned.contains(kw);
}

} // namespace

namespace detail {

struct WriterEngine {
    QSqlDatabase db;
    const Clock* clock = nullptr;
    QHash<QString, QSqlQuery> cache;
    bool syncFull = false;
    quint64 batches = 0;

    QSqlQuery* prepared(const QString& sql, QString* error) {
        auto it = cache.find(sql);
        if (it == cache.end()) {
            if (cache.size() >= kMaxCachedStatements) cache.clear();   // dynamic SQL texts must not grow it forever
            QSqlQuery q(db);
            q.setForwardOnly(true);
            if (!q.prepare(sql)) {
                if (error) *error = queryError(q);
                return nullptr;
            }
            it = cache.insert(sql, q);
        }
        return &it.value();
    }
    // A statement of the writer itself (transaction control, pragmas): no binds, no result rows.
    bool raw(const QString& sql, QString* error = nullptr) {
        QString err;
        QSqlQuery* q = prepared(sql, &err);
        if (!q) {
            if (error) *error = err;
            return false;
        }
        const bool ok = q->exec();
        if (!ok && error) *error = queryError(*q);
        q->finish();
        return ok;
    }
};

} // namespace detail

// ---------------- Tx ----------------

bool Tx::exec(const QString& sql, const QVariantList& binds) {
    if (m_failed) return false;            // the write is already doomed: run nothing more of it
    const QString kw = leadingKeyword(sql);
    if (forbiddenInWrite(kw)) {
        fail(QStringLiteral("transaction control and pragmas are not allowed inside a write"));
        return false;
    }
    QString err;
    QSqlQuery* q = m_engine->prepared(sql, &err);
    if (!q) {
        fail(err);
        return false;
    }
    // A cached statement keeps the values bound by its previous execution, and Qt has no public way to clear them:
    // with fewer binds this time they would silently fill the missing placeholders. Preparing again starts empty, so
    // the driver reports the parameter count mismatch instead.
    if (q->boundValues().size() > binds.size() && !q->prepare(sql)) {
        fail(queryError(*q));
        m_engine->cache.remove(sql);
        return false;
    }
    for (int i = 0; i < binds.size(); ++i) q->bindValue(i, binds[i]);
    if (!q->exec()) {
        fail(queryError(*q));
        q->finish();
        return false;
    }
    m_rowsAffected = q->numRowsAffected();
    const QVariant id = q->lastInsertId();
    if (id.isValid() && m_rowsAffected > 0 && (kw == u"INSERT" || kw == u"REPLACE")) m_lastInsertId = id.toLongLong();
    q->finish();
    return true;
}

QSqlQuery Tx::select(const QString& sql, const QVariantList& binds) {
    QSqlQuery q(m_engine->db);
    if (m_failed) return q;
    if (forbiddenInWrite(leadingKeyword(sql))) {
        fail(QStringLiteral("transaction control and pragmas are not allowed inside a write"));
        return QSqlQuery(m_engine->db);
    }
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        fail(queryError(q));
        return QSqlQuery(m_engine->db);
    }
    for (int i = 0; i < binds.size(); ++i) q.bindValue(i, binds[i]);
    if (!q.exec()) {
        fail(queryError(q));
        return QSqlQuery(m_engine->db);
    }
    return q;
}

void Tx::fail(const QString& reason) {
    if (m_failed) return;
    m_failed = true;
    m_error = reason.trimmed().isEmpty() ? QStringLiteral("write failed") : reason;
}

qint64 Tx::nowMs() const { return m_engine->clock->nowMs(); }

QSqlDatabase Tx::db() const { return m_engine->db; }

Write combine(QList<Write> parts, const QString& tag) {
    Write w;
    w.tag = tag;
    QStringList tags;
    for (const Write& p : parts) {
        if (p.durability == Durability::Durable) w.durability = Durability::Durable;
        if (!p.tag.isEmpty()) tags << p.tag;
    }
    if (w.tag.isEmpty()) w.tag = tags.join(u'+');
    w.fn = [parts = std::move(parts)](Tx& tx) {
        for (const Write& p : parts) {
            if (!p.fn) continue;
            p.fn(tx);
            if (tx.failed()) return;
        }
    };
    return w;
}

// ---------------- DbWriter ----------------

DbWriter::DbWriter(Options o) : m_opt(std::move(o)), m_clock(m_opt.clock ? m_opt.clock : &Clock::system()) {
    static std::atomic<quint64> counter{0};
    m_connName = QStringLiteral("studio-dbwriter-%1").arg(++counter);
    m_opt.batchWindowMs = qMax(0, m_opt.batchWindowMs);
    m_opt.maxBatchOps = qMax(1, m_opt.maxBatchOps);
    m_opt.maxQueue = qMax(1, m_opt.maxQueue);
}

DbWriter::~DbWriter() {
    // Freeing this object under a writer thread that still uses it would be a use-after-free: past the deadline, wait
    // for it without one. Db never gets here with a stuck writer (it leaks it), so only a DbWriter owned directly can.
    if (!stop()) stop(-1);
}

bool DbWriter::start(QString* error) {
    {
        std::lock_guard lk(m_mutex);
        if (m_thread) {
            if (error) *error = QStringLiteral("the writer was already started");
            return false;
        }
        m_startError.clear();
        m_started = m_stopping = m_threadDone = false;
    }
    m_thread = QThread::create([this] { threadMain(); });
    m_thread->setObjectName(QStringLiteral("studio-dbwriter"));
    m_thread->start();
    std::unique_lock lk(m_mutex);
    m_progress.wait(lk, [&] { return m_started || m_threadDone; });
    if (m_started) {
        m_accepting = true;
        return true;
    }
    const QString why = m_startError.isEmpty() ? QStringLiteral("the writer thread ended") : m_startError;
    lk.unlock();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    if (error) *error = why;
    return false;
}

bool DbWriter::running() const {
    std::lock_guard lk(m_mutex);
    return m_accepting;
}

QObject* DbWriter::dispatcherFor(QThread* t) {
    std::lock_guard lk(m_dispMutex);
    auto it = m_dispatchers.find(t);
    if (it != m_dispatchers.end() && it->second.thread) return it->second.obj;
    // A QThread object at a recycled address: its old relay lives in a finished thread and would never deliver.
    auto* d = new QObject;
    d->setObjectName(QStringLiteral("studio-dbwriter-relay"));
    if (d->thread() != t) d->moveToThread(t);
    m_dispatchers[t] = Dispatcher{QPointer<QThread>(t), d};
    return d;
}

bool DbWriter::submit(Write w, QObject* context, Callback cb) {
    if (!w.fn) {
        std::lock_guard lk(m_mutex);
        ++m_stats.refused;
        return false;
    }
    QObject* relay = context ? dispatcherFor(context->thread()) : nullptr;
    bool wake = false;
    {
        std::lock_guard lk(m_mutex);
        if (!m_accepting || int(m_queue.size()) >= m_opt.maxQueue) {
            ++m_stats.refused;
            return false;
        }
        Item it;
        it.w = std::move(w);
        it.context = context;
        it.dispatcher = relay;
        it.cb = std::move(cb);
        it.seq = ++m_nextSeq;
        it.enqueuedSteadyMs = steadyNowMs();
        const bool durable = it.w.durability == Durability::Durable;
        if (durable) ++m_durableQueued;
        wake = durable || m_queue.empty() || int(m_queue.size()) + 1 >= m_opt.maxBatchOps;
        m_queue.push_back(std::move(it));
        ++m_stats.submitted;
        m_stats.queued = int(m_queue.size());
    }
    if (wake) m_wake.notify_one();
    return true;
}

bool DbWriter::commitThenSend(Write w, QObject* context, Callback send, Callback failed) {
    Q_ASSERT_X(context, "DbWriter::commitThenSend", "a context object is required");
    if (!context || !send) return false;
    w.durability = Durability::Durable;
    return submit(std::move(w), context, [send = std::move(send), failed = std::move(failed)](const WriteResult& r) {
        if (r.ok) send(r);
        else if (failed) failed(r);
    });
}

bool DbWriter::flush(int timeoutMs) {
    std::unique_lock lk(m_mutex);
    if (!m_started || m_threadDone) return false;
    const quint64 target = m_nextSeq;
    if (m_doneSeq >= target) return true;
    m_flushTarget = qMax(m_flushTarget, target);
    m_wake.notify_one();
    m_progress.wait_for(lk, std::chrono::milliseconds(qMax(0, timeoutMs)),
                        [&] { return m_doneSeq >= target || m_threadDone; });
    return m_doneSeq >= target;
}

bool DbWriter::stop(int timeoutMs) {
    {
        std::lock_guard lk(m_mutex);
        if (!m_thread) return true;
        m_accepting = false;
        m_stopping = true;
    }
    m_wake.notify_all();
    const QDeadlineTimer deadline = timeoutMs < 0 ? QDeadlineTimer(QDeadlineTimer::Forever) : QDeadlineTimer(timeoutMs);
    if (!m_thread->wait(deadline)) {
        // Stuck (a write blocked in I/O, a write that never returns). The thread still drains the queue, writes the
        // seq counters and stats, uses the connection and posts callbacks through the relays: free none of it. The
        // owner must leak this object (Db::close does) or call stop() again; the destructor waits.
        log::error(kCat, QStringLiteral("the writer did not stop within %1 ms; it is left running").arg(timeoutMs));
        return false;
    }
    delete m_thread;
    m_thread = nullptr;
    std::lock_guard lk(m_dispMutex);
    for (auto& [t, d] : m_dispatchers) d.obj->deleteLater();   // after the callbacks already posted to it
    m_dispatchers.clear();
    return true;
}

DbWriter::Stats DbWriter::stats() const {
    std::lock_guard lk(m_mutex);
    return m_stats;
}

void DbWriter::threadMain() {
    QString err;
    {
        auto engine = std::make_unique<detail::WriterEngine>();
        engine->clock = m_clock;
        engine->db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connName);
        if (!engine->db.isValid()) {
            err = QStringLiteral("the Qt SQLite driver (QSQLITE) is not available");
        } else {
            engine->db.setDatabaseName(m_opt.path);
            engine->db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(qMax(0, m_opt.busyTimeoutMs)));
            if (!engine->db.open())
                err = QStringLiteral("cannot open %1: %2").arg(QDir::toNativeSeparators(m_opt.path), engine->db.lastError().text());
            else if (!configureConnection(engine->db, true, &err) && err.isEmpty())
                err = QStringLiteral("cannot configure the connection");
        }
        if (err.isEmpty()) m_engine = std::move(engine);
        else if (engine->db.isValid()) engine->db.close();
    }
    if (!err.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connName);
        std::lock_guard lk(m_mutex);
        m_startError = err;
        m_threadDone = true;
        m_progress.notify_all();
        return;
    }
    {
        std::lock_guard lk(m_mutex);
        m_started = true;
    }
    m_progress.notify_all();

    std::unique_lock lk(m_mutex);
    for (;;) {
        m_wake.wait(lk, [&] { return !m_queue.empty() || m_stopping; });
        if (m_queue.empty()) break;        // stopping, and everything is committed
        // Gather for up to batchWindowMs after the oldest queued write, unless something needs it committed now.
        for (;;) {
            if (m_stopping || m_durableQueued > 0 || m_flushTarget > m_doneSeq || int(m_queue.size()) >= m_opt.maxBatchOps)
                break;
            const qint64 left = m_queue.front().enqueuedSteadyMs + m_opt.batchWindowMs - steadyNowMs();
            if (left <= 0) break;
            m_wake.wait_for(lk, std::chrono::milliseconds(left));
        }
        std::deque<Item> batch;
        const int take = qMin(int(m_queue.size()), m_opt.maxBatchOps);
        for (int i = 0; i < take; ++i) {
            if (m_queue.front().w.durability == Durability::Durable) --m_durableQueued;
            batch.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        m_stats.queued = int(m_queue.size());
        const quint64 lastSeq = batch.back().seq;
        lk.unlock();
        runBatch(batch);
        lk.lock();
        m_doneSeq = lastSeq;
        m_progress.notify_all();
    }
    lk.unlock();

    // Shutdown: fold the WAL back into the database so the files left behind are small and self-contained.
    m_engine->raw(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    m_engine->cache.clear();
    m_engine->db.close();
    m_engine.reset();
    QSqlDatabase::removeDatabase(m_connName);
    std::lock_guard g(m_mutex);
    m_threadDone = true;
    m_progress.notify_all();
}

void DbWriter::runBatch(std::deque<Item>& items) {
    detail::WriterEngine& e = *m_engine;
    const quint64 batchNo = ++e.batches;
    bool durable = false;
    for (const Item& it : items) durable = durable || it.w.durability == Durability::Durable;
    QElapsedTimer timer;
    timer.start();

    QString batchError;
    // synchronous=FULL fsyncs the WAL at COMMIT: a money row survives a power cut once its callback has run.
    if (durable != e.syncFull) {
        if (e.raw(durable ? QStringLiteral("PRAGMA synchronous = FULL") : QStringLiteral("PRAGMA synchronous = NORMAL"), &batchError))
            e.syncFull = durable;
        else if (durable)
            batchError = QStringLiteral("cannot switch to synchronous=FULL: ") + batchError;   // money: fail closed
        else
            batchError.clear();            // staying on FULL is only slower
    }
    std::vector<WriteResult> results(items.size());
    QString err;
    if (batchError.isEmpty() && !e.raw(QStringLiteral("BEGIN IMMEDIATE"), &err))
        batchError = QStringLiteral("cannot begin the batch transaction: ") + err;
    if (batchError.isEmpty()) {
        for (size_t i = 0; i < items.size(); ++i) {
            WriteResult& r = results[i];
            if (!e.raw(QStringLiteral("SAVEPOINT w"), &err)) {
                batchError = QStringLiteral("cannot open a savepoint: ") + err;
                break;
            }
            Tx tx(&e);
            try {
                items[i].w.fn(tx);
            } catch (const std::exception& ex) {
                tx.fail(QStringLiteral("exception: %1").arg(QString::fromLocal8Bit(ex.what())));
            } catch (...) {
                tx.fail(QStringLiteral("unknown exception"));
            }
            if (tx.failed()) {
                // ROLLBACK TO fails when SQLite already rolled the whole transaction back (I/O error, disk full, busy):
                // then every earlier write of the batch is gone too, so the batch fails as a whole.
                if (!e.raw(QStringLiteral("ROLLBACK TO w"), &err) || !e.raw(QStringLiteral("RELEASE w"), &err)) {
                    batchError = QStringLiteral("the batch transaction was lost after a failed write (%1): %2").arg(tx.error(), err);
                    break;
                }
                r.error = tx.error();
            } else if (!e.raw(QStringLiteral("RELEASE w"), &err)) {
                batchError = QStringLiteral("cannot release a savepoint: ") + err;
                break;
            } else {
                r.ok = true;
                r.value = tx.m_value;
                r.lastInsertId = tx.m_lastInsertId;
            }
            if (m_opt.afterWriteHook) m_opt.afterWriteHook(batchNo, int(i));
        }
        if (batchError.isEmpty() && !e.raw(QStringLiteral("COMMIT"), &err))
            batchError = QStringLiteral("commit failed: ") + err;
        if (!batchError.isEmpty()) e.raw(QStringLiteral("ROLLBACK"));
    }
    const qint64 committedMs = m_clock->nowMs();
    int okCount = 0;
    for (WriteResult& r : results) {
        r.batch = batchNo;
        if (!batchError.isEmpty()) {
            r.ok = false;
            r.error = batchError;
            r.value = QVariant();
            r.lastInsertId = -1;
        }
        r.durable = r.ok && durable;
        r.committedMs = r.ok ? committedMs : 0;
        if (r.ok) ++okCount;
    }
    const qint64 ms = timer.elapsed();
    {
        std::lock_guard lk(m_mutex);
        ++m_stats.batches;
        if (durable && batchError.isEmpty()) ++m_stats.durableBatches;
        if (!batchError.isEmpty()) ++m_stats.failedBatches;
        m_stats.committed += quint64(okCount);
        m_stats.failed += quint64(results.size()) - quint64(okCount);
        m_stats.lastBatchWrites = int(results.size());
        m_stats.lastBatchMs = ms;
        m_stats.maxBatchMs = qMax(m_stats.maxBatchMs, ms);
    }
    if (!batchError.isEmpty())
        log::error(kCat, QStringLiteral("batch %1 (%2 writes) failed: %3").arg(batchNo).arg(results.size()).arg(batchError));
    else if (ms > m_opt.slowBatchMs)
        log::warn(kCat, QStringLiteral("slow batch %1: %2 writes in %3 ms").arg(batchNo).arg(results.size()).arg(ms));
    for (size_t i = 0; i < items.size(); ++i) {
        if (!results[i].ok && batchError.isEmpty())
            log::warn(kCat, QStringLiteral("write %1 failed: %2")
                                .arg(items[i].w.tag.isEmpty() ? QStringLiteral("(untagged)") : items[i].w.tag, results[i].error));
        deliver(items[i], results[i]);
    }
}

void DbWriter::deliver(Item& item, const WriteResult& r) {
    if (!item.cb) return;
    if (!item.dispatcher) {
        try {
            item.cb(r);
        } catch (...) {
            log::error(kCat, QStringLiteral("a write callback threw on the writer thread"));
        }
        return;
    }
    // The relay lives in the context's thread, so the QPointer check below runs where the context could be deleted:
    // no race with its destruction.
    QMetaObject::invokeMethod(item.dispatcher, [ctx = item.context, cb = std::move(item.cb), r] {
        if (ctx) cb(r);
    }, Qt::QueuedConnection);
}

} // namespace studio::store
