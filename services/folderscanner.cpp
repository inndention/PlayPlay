#include "folderscanner.h"
#include "services/metadataservice.h"
#include <QDebug>
#include <QDateTime>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <random>

const QStringList FolderScanner::MEDIA_EXTENSIONS = {
    ".mp4", ".mov", ".avi", ".mkv", ".m4v",
    ".mp3", ".wav", ".aac", ".m4a",
    ".jpg", ".jpeg", ".png", ".gif"
};

FolderScanner::FolderScanner(QObject* parent)
    : QObject(parent)
{
}

QFuture<QList<ScannedFile>> FolderScanner::scanFolder(const QString& folderPath)
{
    return QtConcurrent::run([this, folderPath]() {
        return scanFolderSync(folderPath);
    });
}

QList<ScannedFile> FolderScanner::scanFolderSync(const QString& folderPath)
{
    QList<ScannedFile> files;
    QDir dir(folderPath);
    
    if (!dir.exists()) {
        emit scanError(folderPath, "Folder does not exist");
        return files;
    }
    
    QStringList filters;
    for (const QString& ext : MEDIA_EXTENSIONS) {
        filters << "*" + ext;
    }
    
    QFileInfoList fileInfos = dir.entryInfoList(filters, QDir::Files | QDir::NoSymLinks);
    
    for (const QFileInfo& fileInfo : fileInfos) {
        if (isMediaFile(fileInfo.filePath())) {
            ScannedFile file;
            file.path = fileInfo.absoluteFilePath();
            file.name = fileInfo.fileName();
            file.size = fileInfo.size();
            file.modified = fileInfo.lastModified().toSecsSinceEpoch();
            file.extension = fileInfo.suffix();
            files.append(file);
        }
    }
    
    emit folderScanned(folderPath, files);
    return files;
}

QList<ScannedFile> FolderScanner::sortFiles(const QList<ScannedFile>& files, FolderSortOrder order)
{
    QList<ScannedFile> sorted = files;
    
    switch (order) {
        case FolderSortOrder::Name:
            std::sort(sorted.begin(), sorted.end(), [](const ScannedFile& a, const ScannedFile& b) {
                return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
            });
            break;
        case FolderSortOrder::Date:
            std::sort(sorted.begin(), sorted.end(), [](const ScannedFile& a, const ScannedFile& b) {
                return a.modified > b.modified; // Newest first
            });
            break;
        case FolderSortOrder::Size:
            std::sort(sorted.begin(), sorted.end(), [](const ScannedFile& a, const ScannedFile& b) {
                return a.size > b.size; // Largest first
            });
            break;
        case FolderSortOrder::Random:
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(sorted.begin(), sorted.end(), g);
            break;
    }
    
    return sorted;
}

QFuture<QJsonArray> FolderScanner::convertToPlaylistItems(const QList<ScannedFile>& files, const QString& folderPath)
{
    return QtConcurrent::run([files, folderPath]() {
        QJsonArray items;
        
        for (const ScannedFile& file : files) {
            QJsonObject mediaItem;
            QString safeId = file.path;
            safeId.replace(QRegularExpression("[^a-zA-Z0-9]"), "_");
            mediaItem["id"] = QString("folder_%1").arg(safeId);
            mediaItem["title"] = file.name;
            mediaItem["duration"] = 0; // Will be filled by metadata extraction
            mediaItem["src"] = file.path;
            mediaItem["type"] = (file.extension.toLower() == "jpg" || file.extension.toLower() == "jpeg" || 
                                file.extension.toLower() == "png" || file.extension.toLower() == "gif") ? "image" : "video";
            mediaItem["filePath"] = file.path;
            mediaItem["filename"] = file.name;
            
            items.append(mediaItem);
        }
        
        return items;
    });
}

bool FolderScanner::isMediaFile(const QString& filePath)
{
    QString ext = QFileInfo(filePath).suffix().toLower();
    return MEDIA_EXTENSIONS.contains("." + ext);
}

QJsonObject ScannedFile::toJson() const
{
    QJsonObject json;
    json["path"] = path;
    json["name"] = name;
    json["size"] = size;
    json["modified"] = modified;
    json["extension"] = extension;
    return json;
}

ScannedFile ScannedFile::fromJson(const QJsonObject& json)
{
    ScannedFile file;
    file.path = json["path"].toString();
    file.name = json["name"].toString();
    file.size = json["size"].toVariant().toLongLong();
    file.modified = json["modified"].toVariant().toLongLong();
    file.extension = json["extension"].toString();
    return file;
}

