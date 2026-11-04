#ifndef ACTIVITYLOGMODEL_H
#define ACTIVITYLOGMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "database/asrunlogger.h"

class ActivityLogModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TimeRole = Qt::UserRole + 1,
        EventRole,
        StatusRole
    };

    explicit ActivityLogModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void refresh();

private:
    QList<AsRunEntry> m_entries;
    AsRunLogger* m_logger;
};

#endif // ACTIVITYLOGMODEL_H
