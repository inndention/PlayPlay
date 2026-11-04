#include "storereader.h"
#include "schedulestore.h"
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QQueue>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonArray>
#include <QUuid>
#include <QFileInfo>
#include <QPointer>
#include <QMetaObject>
#include <QDebug>
#include <atomic>

namespace sched {

namespace {
struct Job { StoreReader::Work work; StoreReader::Done done; };

struct Shared {
    QMutex m;
    QWaitCondition cv;
    QQueue<Job> q;
    bool quit = false;
    int running = 0;
    int openOk = 0, openFailed = 0;
    quint64 completed = 0;
    QWaitCondition opened;   // workers report their open result
};
}

struct StoreReader::Impl {
    QString path;
    QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    int drainMs = 5000;
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    QList<QThread*> workers;
};

namespace {
// One worker: opens its own read-only connection, serves the shared queue FIFO, closes the connection on its thread.
class ReaderThread : public QThread {
public:
    ReaderThread(std::shared_ptr<Shared> sh, QString path, QString conn, QPointer<QObject> owner)
        : m_sh(std::move(sh)), m_path(std::move(path)), m_conn(std::move(conn)), m_owner(owner) {}
protected:
    void run() override {
        bool ok = false;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_conn);
            db.setDatabaseName(m_path);
            db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
            ok = QFileInfo::exists(m_path) && db.open();
            if (ok) { QSqlQuery q(db); q.exec("PRAGMA query_only=1"); }
            else qWarning() << "[StoreReader] cannot open" << m_path << "read-only:" << db.lastError().text();
            {
                QMutexLocker l(&m_sh->m);
                if (ok) ++m_sh->openOk; else ++m_sh->openFailed;
                m_sh->opened.wakeAll();
            }
            if (ok) loop(db);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_conn);
    }
private:
    void loop(QSqlDatabase& db) {
        for (;;) {
            Job job;
            {
                QMutexLocker l(&m_sh->m);
                while (!m_sh->quit && m_sh->q.isEmpty()) m_sh->cv.wait(&m_sh->m);
                if (m_sh->quit) return;   // queued jobs are dropped on shutdown
                job = m_sh->q.dequeue();
                ++m_sh->running;
            }
            QJsonObject result;
            try { result = job.work ? job.work(db) : QJsonObject(); }
            catch (const std::exception& e) { result = QJsonObject{{"error", QString("reader threw: ") + e.what()}, {"status", 500}}; }
            catch (...) { result = QJsonObject{{"error", "reader threw"}, {"status", 500}}; }
            QMutexLocker l(&m_sh->m);
            --m_sh->running; ++m_sh->completed;
            // Delivery under the lock: the destructor sets quit under the same lock before it waits, so no call is
            // posted to an owner that is going away (and a posted one dies with the QObject anyway).
            if (!m_sh->quit && m_owner && job.done) {
                StoreReader::Done done = std::move(job.done);
                QMetaObject::invokeMethod(m_owner, [done, result]() { done(result); }, Qt::QueuedConnection);
            }
        }
    }
    std::shared_ptr<Shared> m_sh;
    QString m_path, m_conn;
    QPointer<QObject> m_owner;
};
}

StoreReader::StoreReader(const QString& dbPath, int threads, QObject* parent) : QObject(parent), d(new Impl) {
    d->path = dbPath;
    threads = qBound(1, threads, 8);
    for (int i = 0; i < threads; ++i) {
        auto* t = new ReaderThread(d->sh, dbPath, QString("ppreader_%1_%2").arg(d->uid).arg(i), QPointer<QObject>(this));
        t->setObjectName(QString("StoreReader-%1").arg(i));
        d->workers.append(t);
        t->start();
    }
    // Wait for the open results (a local file open: milliseconds) so run() can answer honestly from the first call.
    QMutexLocker l(&d->sh->m);
    while (d->sh->openOk + d->sh->openFailed < threads) if (!d->sh->opened.wait(&d->sh->m, 3000)) break;
    if (d->sh->openOk > 0) qInfo() << "[StoreReader]" << d->sh->openOk << "read-only reader(s) on" << dbPath;
}

StoreReader::~StoreReader() {
    {
        QMutexLocker l(&d->sh->m);
        d->sh->quit = true;
        d->sh->q.clear();
        d->sh->cv.wakeAll();
    }
    for (QThread* t : d->workers) {
        if (t->wait(d->drainMs)) delete t;
        else { qWarning() << "[StoreReader] a query is still running after" << d->drainMs << "ms; leaving its thread"; }
    }
}

bool StoreReader::run(Work work, Done done) {
    QMutexLocker l(&d->sh->m);
    if (d->sh->quit || d->sh->openOk == 0) return false;
    d->sh->q.enqueue({std::move(work), std::move(done)});
    d->sh->cv.wakeOne();
    return true;
}

bool StoreReader::isOpen() const { QMutexLocker l(&d->sh->m); return !d->sh->quit && d->sh->openOk > 0; }
int StoreReader::threads() const { QMutexLocker l(&d->sh->m); return d->sh->openOk; }
int StoreReader::queued() const { QMutexLocker l(&d->sh->m); return d->sh->q.size(); }
int StoreReader::running() const { QMutexLocker l(&d->sh->m); return d->sh->running; }
quint64 StoreReader::completed() const { QMutexLocker l(&d->sh->m); return d->sh->completed; }
void StoreReader::setDrainMs(int ms) { d->drainMs = qMax(0, ms); }

// --- queries ----------------------------------------------------------------------------------------------------------

QJsonObject StoreReader::notOpen() { return QJsonObject{{"error", "store reader not open"}, {"status", 503}}; }

QList<AsRunRow> StoreReader::asRun(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs, int limit) {
    QList<AsRunRow> out;
    if (!db.isOpen()) return out;
    QSqlQuery q(db);
    QString sql = "SELECT * FROM as_run WHERE actual_start >= ? AND actual_start <= ?";
    if (!channelId.isEmpty()) sql += " AND channel_id = ?";
    sql += " ORDER BY actual_start ASC, id ASC LIMIT ?";
    q.prepare(sql);
    q.addBindValue(fromMs); q.addBindValue(toMs);
    if (!channelId.isEmpty()) q.addBindValue(channelId);
    q.addBindValue(limit);
    if (q.exec()) while (q.next()) out.append(ScheduleStore::asRunFromQuery(q));
    else qWarning() << "[StoreReader] as_run query failed:" << q.lastError().text();
    return out;
}

QJsonObject StoreReader::asRunReport(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs, int limit) {
    if (!db.isOpen()) return notOpen();
    QJsonObject o; QJsonArray a;
    for (const auto& r : asRun(db, channelId, fromMs, toMs, limit)) a.append(r.toJson());
    o["rows"] = a; o["count"] = a.size(); o["fromMs"] = double(fromMs); o["toMs"] = double(toMs);
    o["summary"] = ScheduleStore::summarizeAsRun(asRun(db, channelId, fromMs, toMs, 100000));
    return o;
}

QJsonObject StoreReader::asRunSummary(QSqlDatabase& db, const QString& channelId, qint64 fromMs, qint64 toMs) {
    if (!db.isOpen()) return notOpen();
    return ScheduleStore::summarizeAsRun(asRun(db, channelId, fromMs, toMs, 100000));
}

} // namespace sched
