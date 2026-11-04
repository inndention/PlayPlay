#ifndef PLAYLISTMODEL_H
#define PLAYLISTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "core/types.h"

#include "database/mediadb.h"

class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int currentPlayingRow READ currentPlayingRow WRITE setCurrentPlayingRow NOTIFY currentPlayingRowChanged)

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        StartTimeRole,
        EndTimeRole,
        DurationRole,
        StatusRole,
        PlaylistItemRole,
        AssetIdRole,
        AssetPathRole,
        IsPlayingRole
    };
    Q_ENUM(Role)

    explicit PlaylistModel(QObject* parent = nullptr);
    
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    void setPlaylist(const QList<PlaylistItem>& items);
    
    // Actions
    Q_INVOKABLE void loadSlot(int day, int hour);
    Q_INVOKABLE void saveSlot();
    
    Q_INVOKABLE void addItem(const PlaylistItem& item);
    Q_INVOKABLE void addItemFromMedia(const QVariant& mediaItemVariant);
    Q_INVOKABLE void removeItem(int row);
    Q_INVOKABLE void moveItem(int fromRow, int toRow);
    PlaylistItem getItem(int row) const;
    int currentPlayingRow() const { return m_currentPlayingRow; }
    void setCurrentPlayingRow(int row);
    int rowForAssetId(const QString& assetId) const;

signals:
    void currentPlayingRowChanged();
    void playlistChanged();

private:
    QString formatDuration(double seconds) const;
    void recalculateTiming();
    void updateEngineSchedule();
    
    MediaDb* m_mediaDb;
    QList<PlaylistItem> m_playlistItems;
    QString m_currentSlotId;
    int m_currentDay = -1;
    int m_currentHour = -1;
    int m_currentPlayingRow = -1;
};

#endif // PLAYLISTMODEL_H

