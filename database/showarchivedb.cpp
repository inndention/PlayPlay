#include <atomic>
#include "showarchivedb.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>

ShowArchiveDb::ShowArchiveDb(QObject* parent)
    : QObject(parent)
{
}

ShowArchiveDb::~ShowArchiveDb()
{
    close();
}

bool ShowArchiveDb::initialize()
{
    QString userDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(userDataPath);
    
    QString dbFile = userDataPath + "/show_archives.db";
    static std::atomic<int> serial{0};
    m_db = QSqlDatabase::addDatabase("QSQLITE", QString("ShowArchiveDb-%1").arg(++serial));
    m_db.setDatabaseName(dbFile);
    
    if (!m_db.open()) {
        qWarning() << "Failed to open show archive database:" << m_db.lastError().text();
        return false;
    }
    
    QSqlQuery query(m_db);
    query.exec("PRAGMA journal_mode = WAL");
    
    return ensureTables();
}

void ShowArchiveDb::close()
{
    if (m_db.isValid()) {
        const QString name = m_db.connectionName();
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

bool ShowArchiveDb::ensureTables()
{
    QSqlQuery query(m_db);
    
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS show_archives (
            id TEXT PRIMARY KEY,
            title TEXT,
            start_time INTEGER,
            end_time INTEGER,
            duration REAL,
            segments TEXT
        )
    )");
    
    return true;
}

