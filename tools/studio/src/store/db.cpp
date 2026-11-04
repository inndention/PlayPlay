#include "store/db.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <atomic>

#include "core/clock.h"
#include "core/log.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace studio::store {
namespace {

constexpr auto kCat = u"store";
std::atomic<quint64> g_connCounter{0};

QString queryError(const QSqlQuery& q) {
    const QSqlError e = q.lastError();
    const QString t = e.databaseText().trimmed();
    return t.isEmpty() ? e.text().trimmed() : t;
}

bool pragma(QSqlDatabase& db, const QString& sql, QString* value, QString* error) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        if (error) *error = sql + QStringLiteral(": ") + queryError(q);
        return false;
    }
    if (value) *value = q.next() ? q.value(0).toString() : QString();
    return true;
}

// "3.50.2" >= 3.37.0 (STRICT tables).
bool versionAtLeast(const QString& v, int major, int minor) {
    const QStringList p = v.split(u'.');
    if (p.size() < 2) return false;
    const int a = p[0].toInt(), b = p[1].toInt();
    return a > major || (a == major && b >= minor);
}

} // namespace

// ---------------- connections ----------------

ScopedConnection::ScopedConnection(const QString& path, Mode mode, int busyTimeoutMs) {
    m_name = QStringLiteral("studio-store-%1-%2")
                 .arg(mode == Mode::ReadOnly ? QStringLiteral("r") : QStringLiteral("w"))
                 .arg(++g_connCounter);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
    if (!m_db.isValid()) {
        m_error = QStringLiteral("the Qt SQLite driver (QSQLITE) is not available");
        return;
    }
    m_db.setDatabaseName(path);
    QString opts = QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(qMax(0, busyTimeoutMs));
    if (mode == Mode::ReadOnly) opts += QStringLiteral(";QSQLITE_OPEN_READONLY");
    m_db.setConnectOptions(opts);
    if (!m_db.open()) {
        m_error = QStringLiteral("cannot open %1: %2").arg(QDir::toNativeSeparators(path), m_db.lastError().text());
        return;
    }
}

ScopedConnection::~ScopedConnection() {
    if (m_db.isValid()) m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_name);
}

bool configureConnection(QSqlDatabase& db, bool writer, QString* error) {
    if (!writer) return pragma(db, QStringLiteral("PRAGMA query_only = 1"), nullptr, error);
    QString mode;
    if (!pragma(db, QStringLiteral("PRAGMA journal_mode = WAL"), &mode, error)) return false;
    if (mode.compare(u"wal", Qt::CaseInsensitive) != 0) {
        if (error) *error = QStringLiteral("journal_mode stays '%1' instead of 'wal' (a network or read-only volume?)").arg(mode);
        return false;
    }
    return pragma(db, QStringLiteral("PRAGMA synchronous = NORMAL"), nullptr, error)
        && pragma(db, QStringLiteral("PRAGMA temp_store = MEMORY"), nullptr, error)
        && pragma(db, QStringLiteral("PRAGMA journal_size_limit = 67108864"), nullptr, error)
        && pragma(db, QStringLiteral("PRAGMA cache_size = -16000"), nullptr, error);
}

QString sqliteVersion(const QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT sqlite_version()")) || !q.next()) return {};
    return q.value(0).toString();
}

bool isNetworkPath(const QString& path) {
    QString p = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (p.startsWith(u"\\\\?\\") || p.startsWith(u"\\\\.\\")) {
        p = p.mid(4);                                                         // device prefix
        if (p.startsWith(u"UNC\\", Qt::CaseInsensitive)) return true;         // \\?\UNC\server\..., \\.\UNC\server\...
    } else if (p.startsWith(u"\\\\")) {
        return true;                                                          // \\server\share
    }
#ifdef Q_OS_WIN
    if (p.size() >= 2 && p[1] == u':') {
        const std::wstring root = (p.left(2) + QStringLiteral("\\")).toStdWString();
        return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    }
#endif
    return false;
}

// ---------------- Db ----------------

Db::Db(DbOptions o) : m_opt(std::move(o)) {}

Db::~Db() { close(); }

bool Db::open(QString* error) {
    if (m_open) return true;
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        log::error(kCat, QStringLiteral("open refused: ") + why);
        stopWriter();
        m_reader.reset();
        return false;
    };
    const QString path = m_opt.path;
    if (path.isEmpty() || path == u":memory:" || path.startsWith(u"file:"))
        return fail(QStringLiteral("a database file path is required"));
    const QFileInfo fi(path);
    if (!fi.isAbsolute()) return fail(QStringLiteral("the database path must be absolute"));
    if (!m_opt.allowNetworkPath && isNetworkPath(path))
        return fail(QStringLiteral("%1 is on a network drive; SQLite WAL is not safe there (set allowNetworkPath to override)")
                        .arg(QDir::toNativeSeparators(path)));
    if (fi.exists() && !fi.isFile()) return fail(QStringLiteral("%1 is not a file").arg(QDir::toNativeSeparators(path)));
    if (!QDir().mkpath(fi.absolutePath()))
        return fail(QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(fi.absolutePath())));
    const Clock& clock = m_opt.clock ? *m_opt.clock : Clock::system();

    // Migrate on a connection of our own before the writer starts, so no write can see a half-migrated schema.
    {
        ScopedConnection mig(path, ScopedConnection::Mode::ReadWrite, m_opt.busyTimeoutMs);
        if (!mig.isOpen()) return fail(mig.error());
        QSqlDatabase db = mig.db();
        m_sqliteVersion = sqliteVersion(db);
        if (!versionAtLeast(m_sqliteVersion, 3, 37))
            return fail(QStringLiteral("SQLite %1 is too old (3.37 or newer is needed for STRICT tables)").arg(m_sqliteVersion));
        // Migrate first, WAL second: migrate() refuses a foreign database before writing anything, and switching the
        // journal mode would already change its header. An existing Studio database is in WAL mode anyway (persistent).
        QString err;
        if (!migrate(db, m_opt.modules, clock.nowMs(), &m_migrations, &err)) return fail(err);
        if (!configureConnection(db, true, &err)) return fail(err);
    }

    DbWriter::Options wo;
    wo.path = path;
    wo.batchWindowMs = m_opt.batchWindowMs;
    wo.maxBatchOps = m_opt.maxBatchOps;
    wo.maxQueue = m_opt.maxQueue;
    wo.busyTimeoutMs = m_opt.busyTimeoutMs;
    wo.clock = &clock;
    wo.afterWriteHook = m_opt.afterWriteHook;
    m_writer = std::make_unique<DbWriter>(wo);
    QString err;
    if (!m_writer->start(&err)) return fail(err);

    m_reader = std::make_unique<ScopedConnection>(path, ScopedConnection::Mode::ReadOnly, m_opt.busyTimeoutMs);
    if (!m_reader->isOpen()) return fail(m_reader->error());
    {
        QSqlDatabase rdb = m_reader->db();
        if (!configureConnection(rdb, false, &err)) return fail(err);
    }
    m_owner = QThread::currentThread();
    m_open = true;
    log::info(kCat, QStringLiteral("opened %1 (core schema %2, SQLite %3%4)")
                        .arg(QDir::toNativeSeparators(path)).arg(m_migrations.coreVersion).arg(m_sqliteVersion,
                             m_migrations.applied.isEmpty() ? QString()
                                                            : QStringLiteral("; applied ") + m_migrations.applied.join(u", ")));
    return true;
}

void Db::stopWriter() {
    if (!m_writer) return;
    if (!m_writer->stop(m_opt.stopTimeoutMs)) {
        // Its thread still runs on this object: leaked on purpose (it finishes the stuck write, commits and stops by
        // itself), since freeing it here would be a use-after-free and waiting could hang shutdown forever.
        log::error(kCat, QStringLiteral("leaving the stuck writer of %1 running").arg(QDir::toNativeSeparators(m_opt.path)));
        (void)m_writer.release();
    }
    m_writer.reset();
}

void Db::close() {
    stopWriter();
    if (m_reader) {
        Q_ASSERT_X(!m_owner || QThread::currentThread() == m_owner, "store::Db::close", "call it on the thread that opened the Db");
        m_reader.reset();
    }
    if (m_open) log::info(kCat, QStringLiteral("closed %1").arg(QDir::toNativeSeparators(m_opt.path)));
    m_open = false;
    m_owner = nullptr;
}

QSqlDatabase Db::reader() const {
    Q_ASSERT_X(m_reader && QThread::currentThread() == m_owner, "store::Db::reader",
               "only the thread that opened the Db; other threads open a ScopedConnection");
    return m_reader ? m_reader->db() : QSqlDatabase();
}

} // namespace studio::store
