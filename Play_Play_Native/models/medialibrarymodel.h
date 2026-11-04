#ifndef MEDIALIBRARYMODEL_H
#define MEDIALIBRARYMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "core/types.h"
#include "services/fileindexer.h"

class MediaDb;

class MediaLibraryModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        TypeRole,
        DurationRole,
        FilePathRole,
        ThumbnailRole,
        AddedAtRole,
        PlayCountRole,
        MediaItemRole
    };
    Q_ENUM(Role)

    explicit MediaLibraryModel(QObject* parent = nullptr);
    
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    
    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);

    void loadMedia();
    void addMedia(const MediaItem& item);
    void removeMedia(int row);
    MediaItem getMedia(int row) const;

    Q_INVOKABLE QVariant getMediaItem(int row) const;
    Q_INVOKABLE void importFiles();
    Q_INVOKABLE void scanFolder(const QString& path);

signals:
    void mediaLoaded();
    void filterChanged();

private:
    QString formatDuration(double seconds) const;
    void applyFilter();
    
    MediaDb* m_mediaDb;
    FileIndexer* m_fileIndexer;
    QList<MediaItem> m_allMediaItems;
    QList<MediaItem> m_visibleMediaItems;
    QString m_filter;
};

#endif // MEDIALIBRARYMODEL_H

