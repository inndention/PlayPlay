#include "activitylogmodel.h"
#include <QDateTime>

ActivityLogModel::ActivityLogModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_logger(getAsRunLogger())
{
    m_logger->initialize();
    refresh();
}

int ActivityLogModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)
    return m_entries.size();
}

QVariant ActivityLogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size()) {
        return QVariant();
    }

    const AsRunEntry& entry = m_entries.at(index.row());

    switch (role) {
        case TimeRole:
            return QDateTime::fromMSecsSinceEpoch(entry.actualStart).toString("hh:mm:ss");
        case EventRole:
            return entry.mediaTitle.isEmpty() ? entry.eventId : entry.mediaTitle;
        case StatusRole:
            return entry.status;
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> ActivityLogModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[TimeRole] = "time";
    roles[EventRole] = "event";
    roles[StatusRole] = "status";
    return roles;
}

void ActivityLogModel::refresh()
{
    beginResetModel();
    // Query last 50 entries
    QDateTime now = QDateTime::currentDateTime();
    QDateTime dayAgo = now.addDays(-1);
    m_entries = m_logger->queryLogByDateRange(dayAgo, now);
    
    // Sort descending by time
    std::sort(m_entries.begin(), m_entries.end(), [](const AsRunEntry& a, const AsRunEntry& b) {
        return a.actualStart > b.actualStart;
    });

    // Limit to 20
    if (m_entries.size() > 20) {
        m_entries = m_entries.mid(0, 20);
    }
    
    endResetModel();
}
