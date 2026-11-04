#pragma once
// studio.db: opening, migrating, the writer and the readers (ST-03, docs/plans/starmedia-studio.md §4.8).
//
//   Db db({.path = dataDir + "/studio.db"});
//   if (!db.open(&err)) ...;                     // start-up: creates the dir, migrates, starts the DbWriter thread
//   db.writer().submit(jobs::insert(rows), this, [](const WriteResult& r) { ... });   // all writes
//   auto job = jobs::get(db.reader(), id);       // reads on the thread that opened the Db
//
// Readers: SQLite in WAL mode lets readers run beside the writer without blocking either side. db.reader() is a
// read-only connection for the thread that called open() (normally the main loop: small indexed reads only). Another
// thread (a worker doing a heavy export) opens its own ScopedConnection(path, ReadOnly) and drops it when done: Qt SQL
// connections cannot cross threads. A read sees everything committed before it started; finish a QSqlQuery (or let it
// go out of scope) before expecting to see newer commits, since an open SELECT keeps its snapshot.
//
// Refused at open (fail closed): a database on a network drive or UNC path unless allowNetworkPath (WAL needs shared
// memory, which network file systems do not provide reliably, so a second writer could corrupt it), a journal mode that
// will not switch to WAL, a SQLite older than 3.37 (STRICT tables), and every migration refusal (migrations.h).
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QThread>
#include <functional>
#include <memory>

#include "store/dbwriter.h"
#include "store/migrations.h"

namespace studio {
class Clock;
}

namespace studio::store {

struct DbOptions {
    QString path;                          // the studio.db file; its directory is created
    int batchWindowMs = 50;
    int maxBatchOps = 5000;
    int maxQueue = 200000;
    int busyTimeoutMs = 5000;
    int stopTimeoutMs = 30000;             // close(): a writer still busy after this is left running and leaked
    QList<MigrationModule> modules;        // applied after "core", in this order
    bool allowNetworkPath = false;
    const Clock* clock = nullptr;          // migration stamps, Tx::nowMs; nullptr = Clock::system()
    std::function<void(quint64 batch, int index)> afterWriteHook;   // tests only (DbWriter::Options)
};

/// A Qt SQL connection for the calling thread, closed and removed on destruction. Writers other than the DbWriter are
/// for migrations and tests only.
class ScopedConnection {
public:
    enum class Mode { ReadWrite, ReadOnly };
    ScopedConnection(const QString& path, Mode mode, int busyTimeoutMs = 5000);
    ~ScopedConnection();
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;
    bool isOpen() const { return m_db.isOpen(); }
    const QString& error() const { return m_error; }
    QSqlDatabase db() const { return m_db; }
    const QString& name() const { return m_name; }

private:
    QString m_name;
    QSqlDatabase m_db;
    QString m_error;
};

/// Sets the pragmas every Studio connection uses. Writer: journal_mode=WAL (verified), synchronous=NORMAL,
/// temp_store=MEMORY, a 64 MB journal size limit, a 16 MB page cache. Reader: query_only.
bool configureConnection(QSqlDatabase& db, bool writer, QString* error = nullptr);
/// "3.50.2" of the SQLite behind `db` (empty on error).
QString sqliteVersion(const QSqlDatabase& db);
/// True for a UNC path (\\server\share, \\?\UNC\..., \\.\UNC\...) or a path on a mapped network drive.
bool isNetworkPath(const QString& path);

class Db {
public:
    explicit Db(DbOptions o);
    ~Db();                                 // close()
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    /// Creates the directory, migrates, starts the writer and opens the reader of the calling thread. Blocks (start-up
    /// only). False with `error` on any refusal; nothing keeps running then.
    bool open(QString* error = nullptr);
    /// Commits everything queued, stops the writer (WAL checkpoint) and closes the reader. Call it on the thread that
    /// opened the Db. A writer still busy after stopTimeoutMs (a write stuck in I/O) is left running and leaked on
    /// purpose: its thread still uses it, so freeing it would crash; it finishes and stops by itself.
    void close();
    bool isOpen() const { return m_open; }

    /// Only while isOpen().
    DbWriter& writer() {
        Q_ASSERT_X(m_writer, "store::Db::writer", "the Db is not open");
        return *m_writer;
    }
    /// The read-only connection of the thread that called open(). Other threads use their own ScopedConnection.
    QSqlDatabase reader() const;
    const MigrationReport& migrations() const { return m_migrations; }
    const QString& path() const { return m_opt.path; }
    const QString& sqliteVersionText() const { return m_sqliteVersion; }

private:
    void stopWriter();

    DbOptions m_opt;
    bool m_open = false;
    std::unique_ptr<DbWriter> m_writer;
    std::unique_ptr<ScopedConnection> m_reader;
    QThread* m_owner = nullptr;
    MigrationReport m_migrations;
    QString m_sqliteVersion;
};

} // namespace studio::store
