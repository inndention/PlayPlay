#include "playlistmodel.h"
#include "core/types.h"
#include "playout/controlservice.h"
#include <QDebug>

PlaylistModel::PlaylistModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_mediaDb(new MediaDb())
{
    m_mediaDb->initialize();
}

void PlaylistModel::loadSlot(int day, int hour)
{
    beginResetModel();
    m_currentDay = day;
    m_currentHour = hour;
    m_currentSlotId = QString("%1-%2").arg(day).arg(hour);
    
    HourSlot slot = m_mediaDb->getScheduleSlot(m_currentSlotId);
    m_playlistItems = slot.items;
    
    recalculateTiming();
    updateEngineSchedule();
    endResetModel();
}

void PlaylistModel::saveSlot()
{
    if (m_currentDay == -1 || m_currentHour == -1) return;
    
    HourSlot slot;
    slot.id = m_currentSlotId;
    slot.dayOfWeek = m_currentDay;
    slot.hour = m_currentHour;
    slot.items = m_playlistItems;
    // slot.updatedAt = QDateTime::currentDateTime(); // Handled by DB usually
    
    m_mediaDb->saveScheduleSlot(slot);
}

void PlaylistModel::recalculateTiming()
{
    // Simple linear timing calculation starting from XX:00:00
    // In a real system, this would be complex (TimeAuthority ref)
    if (m_currentHour == -1) return;
    
    QDateTime baseTime = QDateTime::currentDateTime(); // Just for date
    QTime startTime(m_currentHour, 0, 0);
    baseTime.setTime(startTime);
    
    for (auto& item : m_playlistItems) {
        item.startTime = baseTime;
        double duration = item.timeOut - item.timeIn;
        if (duration <= 0) duration = item.media.duration; // Fallback
        
        baseTime = baseTime.addSecs(static_cast<qint64>(duration));
        item.endTime = baseTime;
    }
}


int PlaylistModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)
    return m_playlistItems.size();
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_playlistItems.size()) {
        return QVariant();
    }
    
    const PlaylistItem& item = m_playlistItems.at(index.row());
    
    switch (role) {
        case TitleRole:
            return item.media.title;
        case StartTimeRole:
            return item.startTime.isValid() ? item.startTime.toString("hh:mm:ss") : "";
        case EndTimeRole:
            return item.endTime.isValid() ? item.endTime.toString("hh:mm:ss") : "";
        case DurationRole:
            return formatDuration(item.timeOut - item.timeIn);
        case StatusRole:
            return item.isResolved ? tr("Resolved") : tr("Pending");
        case PlaylistItemRole:
            return QVariant::fromValue(item);
        case AssetIdRole:
            return item.media.id;
        case AssetPathRole: {
            if (!item.media.filePath.isEmpty()) return item.media.filePath;
            return item.media.src.toString();
        }
        case IsPlayingRole:
            return index.row() == m_currentPlayingRow;
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[TitleRole] = "title";
    roles[StartTimeRole] = "startTime";
    roles[EndTimeRole] = "endTime";
    roles[DurationRole] = "duration";
    roles[StatusRole] = "status";
    roles[PlaylistItemRole] = "playlistItem";
    roles[AssetIdRole] = "assetId";
    roles[AssetPathRole] = "assetPath";
    roles[IsPlayingRole] = "isPlaying";
    return roles;
}

void PlaylistModel::setPlaylist(const QList<PlaylistItem>& items)
{
    beginResetModel();
    m_playlistItems = items;
    endResetModel();
}

void PlaylistModel::addItem(const PlaylistItem& item)
{
    beginInsertRows(QModelIndex(), m_playlistItems.size(), m_playlistItems.size());
    m_playlistItems.append(item);
    endInsertRows();
    recalculateTiming();
    updateEngineSchedule();
    saveSlot();
}

void PlaylistModel::addItemFromMedia(const QVariant& mediaItemVariant)
{
    MediaItem media = mediaItemVariant.value<MediaItem>();
    if (media.id.isEmpty()) {
        qWarning() << "Attempted to add invalid media to playlist";
        return;
    }

    PlaylistItem newItem;
    newItem.media = media;
    newItem.timeIn = 0;
    newItem.timeOut = media.duration;
    newItem.isResolved = true;
    
    addItem(newItem);
}

void PlaylistModel::removeItem(int row)
{
    if (row < 0 || row >= m_playlistItems.size()) {
        return;
    }
    
    beginRemoveRows(QModelIndex(), row, row);
    m_playlistItems.removeAt(row);
    endRemoveRows();
    recalculateTiming();
    updateEngineSchedule();
    saveSlot();
}

void PlaylistModel::moveItem(int fromRow, int toRow)
{
    if (fromRow < 0 || fromRow >= m_playlistItems.size() ||
        toRow < 0 || toRow >= m_playlistItems.size() ||
        fromRow == toRow) {
        return;
    }
    
    beginMoveRows(QModelIndex(), fromRow, fromRow, QModelIndex(), toRow > fromRow ? toRow + 1 : toRow);
    
    PlaylistItem item = m_playlistItems.takeAt(fromRow);
    m_playlistItems.insert(toRow, item);
    
    endMoveRows();
    recalculateTiming();
    updateEngineSchedule();
    saveSlot();
}

PlaylistItem PlaylistModel::getItem(int row) const
{
    if (row >= 0 && row < m_playlistItems.size()) {
        return m_playlistItems.at(row);
    }
    return PlaylistItem();
}

void PlaylistModel::setCurrentPlayingRow(int row)
{
    if (row == m_currentPlayingRow) return;
    int oldRow = m_currentPlayingRow;
    m_currentPlayingRow = (row >= 0 && row < m_playlistItems.size()) ? row : -1;
    if (oldRow >= 0 && oldRow < m_playlistItems.size()) {
        QModelIndex idx = index(oldRow, 0);
        emit dataChanged(idx, idx, { IsPlayingRole });
    }
    if (m_currentPlayingRow >= 0) {
        QModelIndex idx = index(m_currentPlayingRow, 0);
        emit dataChanged(idx, idx, { IsPlayingRole });
    }
    emit currentPlayingRowChanged();
}

int PlaylistModel::rowForAssetId(const QString& assetId) const
{
    if (assetId.isEmpty()) return -1;
    for (int i = 0; i < m_playlistItems.size(); ++i) {
        if (m_playlistItems.at(i).media.id == assetId) return i;
    }
    return -1;
}

QString PlaylistModel::formatDuration(double seconds) const
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

void PlaylistModel::updateEngineSchedule()
{
    // Map PlaylistItems to PlannedEvents for the AutomationEngine
    QList<PlannedEvent> plannedEvents;
    for (const auto& item : m_playlistItems) {
        PlannedEvent event;
        event.eventId = item.id;
        event.assetId = item.mediaId;
        event.startTime = item.startTime.toMSecsSinceEpoch();
        event.duration = static_cast<qint64>((item.timeOut - item.timeIn) * 1000);
        event.engineId = "main"; // Default engine
        
        QJsonObject meta;
        meta["src"] = item.media.filePath;
        meta["title"] = item.media.title;
        event.metadata = meta;
        
        plannedEvents.append(event);
    }
    
    // Sync with global control service
    getControlService()->updateSchedule(plannedEvents);
    qDebug() << "[PlaylistModel] Synced" << plannedEvents.size() << "events to engine";
}

