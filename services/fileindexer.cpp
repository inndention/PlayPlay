#include "fileindexer.h"
#include "services/logger.h"
#include "core/types.h"
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QDebug>
#include <QStandardPaths>
#include <QUrl>
#include <QDateTime>

FileIndexer::FileIndexer(QObject* parent)
    : QObject(parent)
    , m_isIndexing(false)
    , m_indexedCount(0)
{
    // Supported media file extensions
    m_mediaExtensions << "mp4" << "mov" << "avi" << "mkv" << "mxf" << "mts" 
                      << "mp3" << "wav" << "aac" << "flac" << "m4a"
                      << "jpg" << "jpeg" << "png" << "gif" << "bmp";
}

void FileIndexer::startIndexing(const QString& directory)
{
    if (m_isIndexing) {
        Logger::instance()->warning("File indexing already in progress");
        return;
    }

    if (!QDir(directory).exists()) {
        Logger::instance()->error(QString("Directory does not exist: %1").arg(directory));
        emit error(QString("Directory does not exist: %1").arg(directory));
        return;
    }

    m_currentDirectory = directory;
    m_isIndexing = true;
    m_indexedCount = 0;
    m_filesToIndex.clear();

    Logger::instance()->info(QString("Starting file indexing: %1").arg(directory));
    emit indexingStarted(directory);

    // Find all media files
    QList<QString> files = findMediaFiles(directory);
    m_filesToIndex = files;

    Logger::instance()->info(QString("Found %1 media files").arg(files.size()));

    // Process files
    processNextFile();
}

void FileIndexer::stopIndexing()
{
    if (!m_isIndexing) {
        return;
    }

    m_isIndexing = false;
    Logger::instance()->info("File indexing stopped");
}

bool FileIndexer::isIndexing() const
{
    return m_isIndexing;
}

QList<QString> FileIndexer::findMediaFiles(const QString& directory)
{
    QList<QString> files;
    QDirIterator it(directory, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        QFileInfo fileInfo = it.fileInfo();

        if (fileInfo.isFile()) {
            QString suffix = fileInfo.suffix().toLower();
            if (m_mediaExtensions.contains(suffix)) {
                files.append(fileInfo.absoluteFilePath());
            }
        }
    }

    return files;
}

MediaItem FileIndexer::extractMediaMetadata(const QString& filePath)
{
    MediaItem item;
    QFileInfo fileInfo(filePath);

    // Basic file information
    item.id = QString("media_%1").arg(QDateTime::currentMSecsSinceEpoch());
    item.title = fileInfo.baseName();
    item.filename = fileInfo.fileName();
    item.filePath = fileInfo.absoluteFilePath();
    item.src = QUrl::fromLocalFile(fileInfo.absoluteFilePath()).toString();
    
    // Set type based on extension
    QString suffix = fileInfo.suffix().toLower();
    if (suffix == "mp4" || suffix == "mov" || suffix == "avi" || suffix == "mkv" || suffix == "mxf" || suffix == "mts") {
        item.type = MediaType::Video;
    } else if (suffix == "mp3" || suffix == "wav" || suffix == "aac" || suffix == "flac" || suffix == "m4a") {
        item.type = MediaType::Video; // Audio type not in enum, use Video as fallback
    } else if (suffix == "jpg" || suffix == "jpeg" || suffix == "png" || suffix == "gif" || suffix == "bmp") {
        item.type = MediaType::Image;
    } else {
        item.type = MediaType::Video; // Default
    }

    item.addedAt = QDateTime::currentDateTime();
    item.dateMade = fileInfo.birthTime().isValid() ? fileInfo.birthTime() : fileInfo.lastModified();
    item.duration = 0.0; // TODO: Extract actual duration using ffprobe or similar
    item.playCount = 0;
    item.priority = 0;
    item.archived = false;

    return item;
}

void FileIndexer::processNextFile()
{
    if (!m_isIndexing || m_filesToIndex.isEmpty()) {
        // Finished indexing
        m_isIndexing = false;
        Logger::instance()->info(QString("File indexing finished: %1 files indexed").arg(m_indexedCount));
        emit indexingFinished(m_currentDirectory, m_indexedCount);
        return;
    }

    QString filePath = m_filesToIndex.takeFirst();
    
    // Extract metadata
    MediaItem item = extractMediaMetadata(filePath);
    
    m_indexedCount++;
    
    // Emit progress
    emit indexingProgress(m_indexedCount, m_indexedCount + m_filesToIndex.size(), filePath);
    emit newMediaFound(item);

    // Process next file (with small delay to avoid blocking)
    QTimer::singleShot(10, this, &FileIndexer::processNextFile);
}

