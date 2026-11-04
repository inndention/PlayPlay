#ifndef SHOWARCHIVEDB_H
#define SHOWARCHIVEDB_H

#include <QObject>
#include <QSqlDatabase>

class ShowArchiveDb : public QObject
{
    Q_OBJECT

public:
    explicit ShowArchiveDb(QObject* parent = nullptr);
    ~ShowArchiveDb();
    
    bool initialize();
    void close();

private:
    QSqlDatabase m_db;
    bool ensureTables();
};

#endif // SHOWARCHIVEDB_H

