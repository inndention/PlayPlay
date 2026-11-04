#include "schedulemodel.h"
#include "core/types.h"
#include <QDebug>

ScheduleModel::ScheduleModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ScheduleModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)
    return m_scheduleItems.size();
}

QVariant ScheduleModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_scheduleItems.size()) {
        return QVariant();
    }
    
    const PlaylistItem& item = m_scheduleItems.at(index.row());
    
    switch (role) {
        case TimeRole:
            return item.startTime.isValid() ? item.startTime.toString("hh:mm:ss") : "";
        case TitleRole:
            return item.media.title;
        case DurationRole:
            return formatDuration(item.timeOut - item.timeIn);
        case TypeRole:
            return mediaTypeToString(item.media.type);
        case PlaylistItemRole:
            return QVariant::fromValue(item);
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> ScheduleModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[TimeRole] = "time";
    roles[TitleRole] = "title";
    roles[DurationRole] = "duration";
    roles[TypeRole] = "type";
    roles[PlaylistItemRole] = "playlistItem";
    return roles;
}

void ScheduleModel::setScheduleItems(const QList<PlaylistItem>& items)
{
    beginResetModel();
    m_scheduleItems = items;
    endResetModel();
}

PlaylistItem ScheduleModel::getItem(int row) const
{
    if (row >= 0 && row < m_scheduleItems.size()) {
        return m_scheduleItems.at(row);
    }
    return PlaylistItem();
}

QString ScheduleModel::formatDuration(double seconds) const
{
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;
    
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    
    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

