#ifndef PLAYPLAY_STOREREADER_H
#define PLAYPLAY_STOREREADER_H

#include <QObject>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <functional>
#include <memory>
#include "schedulemodel.h"

namespace sched {

/**
 * StoreReader — read-only SQLite connections on worker threads for the heavy REST GETs (perf-supervisor PS-9 / W15).
 *
 * The ScheduleStore keeps one writer connection on the Supervisor's event loop; /asrun (up to 5,000 rows plus a
 * summary over 100,000) held that loop for the whole query. The reader opens one `QSQLITE_OPEN_READONLY` connection
 * per worker thread on the same WAL database (readers never block the writer and are never blocked by it; a
 * busy_timeout of 5 s covers checkpoints), runs `Work` there and delivers `Done` on the thread the StoreReader
 * lives on (the loop), like BlockingWorker. Jobs run FIFO over `threads` workers.
 *
 * Lifetime: the destructor refuses new jobs, drops queued ones (their Done is never called) and waits up to
 * `drainMs` for running queries; a worker still inside SQLite after that is left to finish on its own (its thread
 * object is leaked rather than terminated inside the library).
 *
 * Safety: the Supervisor falls back to the synchronous store when `run()` returns false (reader not open), so a
 * database that cannot be opened read-only keeps today's behaviour.
 */
class StoreReader : public QObject {
    Q_OBJECT
public:
    using Work = std::function<QJsonObject(QSqlDatabase& db)>;   // reader thread; db is open read-only (check isOpen())
    using Done = std::function<void(const QJsonObject& result)>; // the StoreReader's thread

    explicit StoreReader(const QString& dbPath, int threads = 2, QObject* parent = nullptr);
    ~StoreReader() override;

    /// Queue a job; false when the reader is closed (no worker could open the database, or it is shutting down).
    bool run(Work work, Done done);
    bool isOpen() const;
    int threads() const;
    int queued() const;
    int running() const;
    quint64 completed() const;
    void setDrainMs(int ms);

    // --- the /asrun queries on a given connection (pure; usable from any thread that owns `db`) -------------------
    static QList<AsRunRow> asRun(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs, int limit = 5000);
    /// The GET /api/v1/asrun body: rows, count, fromMs, toMs, summary (summary over up to 100,000 rows as before).
    static QJsonObject asRunReport(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs, int limit = 5000);
    /// The GET /api/v1/asrun/summary body.
    static QJsonObject asRunSummary(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs);
    /// {"error": ..., "status": 503} when the connection is not open (what Work sees after a failed open).
    static QJsonObject notOpen();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace sched

#endif // PLAYPLAY_STOREREADER_H
