#ifndef WEEKLYSCHEDULEMODEL_H
#define WEEKLYSCHEDULEMODEL_H

#include <QAbstractTableModel>
#include "core/types.h"
#include "database/mediadb.h"

class WeeklyScheduleModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Role {
        StatusRole = Qt::UserRole + 1,
        ItemCountRole,
        DurationRole,
        SlotIdRole,     // "day-hour"
        IsLoopingRole,
        ColorRole
    };
    Q_ENUM(Role)

    /*
     * Status levels for a slot:
     * - Empty: No items
     * - Under: Items exist but < 59 mins
     * - Full: 59-61 mins
     * - Over: > 61 mins
     */
    enum SlotStatus {
        Empty,
        Under,
        Full,
        Over
    };
    Q_ENUM(SlotStatus)

    explicit WeeklyScheduleModel(QObject* parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    // Actions
    Q_INVOKABLE void loadSchedule();
    Q_INVOKABLE void copyDay(int sourceDay, int destDay);
    Q_INVOKABLE void clearDay(int day);
    Q_INVOKABLE bool hasData(int day, int hour) const;

private:
    MediaDb* m_mediaDb;
    WeeklySchedule m_scheduleCache;
    
    // Convert 0-6 to "Monday", "Tuesday", etc.
    QString getDayName(int dayIndex) const;
    SlotStatus calculateStatus(const HourSlot& slot) const;
    double calculateDuration(const HourSlot& slot) const;
};

#endif // WEEKLYSCHEDULEMODEL_H
