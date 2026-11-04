#ifndef SCHEDULEMODEL_H
#define SCHEDULEMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "core/types.h"

class ScheduleModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        TimeRole = Qt::UserRole + 1,
        TitleRole,
        DurationRole,
        TypeRole,
        PlaylistItemRole
    };
    Q_ENUM(Role)

    explicit ScheduleModel(QObject* parent = nullptr);
    
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    void setScheduleItems(const QList<PlaylistItem>& items);
    PlaylistItem getItem(int row) const;

signals:
    void scheduleChanged();

private:
    QString formatDuration(double seconds) const;
    
    QList<PlaylistItem> m_scheduleItems;
};

#endif // SCHEDULEMODEL_H

