#include "medialibrarymodel.h"
#include "database/mediadb.h"
#include "core/types.h"
#include <QDebug>
#include <QRandomGenerator>
#include <QDir>
#include <QUrl>
#include <QUuid>
#include <QFileInfo>
#include "services/logger.h"

MediaLibraryModel::MediaLibraryModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_mediaDb(new MediaDb())
    , m_fileIndexer(new FileIndexer(this))
{
    m_mediaDb->initialize();
    
    connect(m_fileIndexer, &FileIndexer::newMediaFound, this, [this](const MediaItem& item) {
        addMedia(item);
    });
    
    loadMedia();
}



int MediaLibraryModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)
    return m_visibleMediaItems.size();
}

QVariant MediaLibraryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_visibleMediaItems.size()) {
        return QVariant();
    }
    
    const MediaItem& item = m_visibleMediaItems.at(index.row());
    
    switch (role) {
        case TitleRole:
            return item.title;
        case TypeRole:
            return mediaTypeToString(item.type);
        case DurationRole:
            return formatDuration(item.duration);
        case FilePathRole:
            return item.filePath;
        case ThumbnailRole:
            if (item.thumbnail.isEmpty()) {
                if (item.type == MediaType::Video) return "qrc:/icons/video_placeholder.png";
                if (item.type == MediaType::Image) return "qrc:/icons/image_placeholder.png";
                return "qrc:/icons/generic_placeholder.png";
            }
            return item.thumbnail;
        case AddedAtRole:
            return item.addedAt.toString(Qt::ISODate);
        case PlayCountRole:
            return item.playCount;
        case MediaItemRole:
            return QVariant::fromValue(item);
        default:
            return QVariant();
    }
}

QHash<int, QByteArray> MediaLibraryModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[TitleRole] = "title";
    roles[TypeRole] = "type";
    roles[DurationRole] = "duration";
    roles[FilePathRole] = "filePath";
    roles[ThumbnailRole] = "thumbnail";
    roles[AddedAtRole] = "addedAt";
    roles[PlayCountRole] = "playCount";
    roles[MediaItemRole] = "mediaItem";
    return roles;
}

void MediaLibraryModel::setFilter(const QString& filter)
{
    if (m_filter != filter) {
        m_filter = filter;
        applyFilter();
        emit filterChanged();
    }
}

void MediaLibraryModel::applyFilter()
{
    beginResetModel();
    m_visibleMediaItems.clear();
    
    if (m_filter.isEmpty()) {
        m_visibleMediaItems = m_allMediaItems;
    } else {
        for (const auto& item : m_allMediaItems) {
            if (item.title.contains(m_filter, Qt::CaseInsensitive)) {
                m_visibleMediaItems.append(item);
            }
        }
    }
    endResetModel();
}

void MediaLibraryModel::loadMedia()
{
    m_allMediaItems = m_mediaDb->getAllMedia();
    applyFilter();
}

void MediaLibraryModel::addMedia(const MediaItem& item)
{
    if (m_mediaDb->saveMedia(item)) {
        m_allMediaItems.append(item);
        applyFilter();
    }
}

void MediaLibraryModel::removeMedia(int row)
{
    if (row < 0 || row >= m_visibleMediaItems.size()) {
        return;
    }
    
    MediaItem item = m_visibleMediaItems.at(row);
    if (m_mediaDb->deleteMedia(item.id)) {
        m_allMediaItems.removeOne(item); // Should use a better ID match, but this works for now
        applyFilter();
    }
}

MediaItem MediaLibraryModel::getMedia(int row) const
{
    if (row >= 0 && row < m_visibleMediaItems.size()) {
        return m_visibleMediaItems.at(row);
    }
    return MediaItem();
}

QVariant MediaLibraryModel::getMediaItem(int row) const
{
    return QVariant::fromValue(getMedia(row));
}

void MediaLibraryModel::scanFolder(const QString& path)
{
    QString localPath = path;
    if (localPath.startsWith("file:///")) {
        localPath = QUrl(localPath).toLocalFile();
    }
    
    QDir dir(localPath);
    if (!dir.exists()) return;
    
    QStringList filters;
    filters << "*.mp4" << "*.mkv" << "*.avi" << "*.mov" << "*.mp3" << "*.wav";
    
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& fileInfo : files) {
        MediaItem item;
        item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.title = fileInfo.baseName();
        item.filePath = fileInfo.absoluteFilePath();
        item.duration = 0; // We'd need an async prober for real duration
        item.type = (fileInfo.suffix().toLower() == "mp3" || fileInfo.suffix().toLower() == "wav") ? MediaType::Audio : MediaType::Video;
        item.addedAt = QDateTime::currentDateTime();
        
        addMedia(item);
    }
    
    Logger::instance()->info(QString("Scanned folder: %1. Found %2 items.").arg(localPath).arg(static_cast<int>(files.size())));
}

void MediaLibraryModel::importFiles()
{
    // Normally we'd use QFileDialog here, but for QML we rely on scanFolder + FileDialog
    qDebug() << "Import Files called (redirected to UI logic)";
}

QString MediaLibraryModel::formatDuration(double seconds) const
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

