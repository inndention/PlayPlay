#ifndef FOLDERSCANNER_H
#define FOLDERSCANNER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QFuture>
#include <QtConcurrent>
#include <QFileInfo>
#include <QDir>

struct ScannedFile {
    QString path;
    QString name;
    qint64 size;
    qint64 modified;
    QString extension;
    
    QJsonObject toJson() const;
    static ScannedFile fromJson(const QJsonObject& json);
};

enum class FolderSortOrder {
    Name,
    Date,
    Size,
    Random
};

class FolderScanner : public QObject
{
    Q_OBJECT
    
public:
    explicit FolderScanner(QObject* parent = nullptr);
    
    // Scan folder for media files
    QFuture<QList<ScannedFile>> scanFolder(const QString& folderPath);
    
    // Sort files by order
    static QList<ScannedFile> sortFiles(const QList<ScannedFile>& files, FolderSortOrder order);
    
    // Convert scanned files to playlist items
    QFuture<QJsonArray> convertToPlaylistItems(const QList<ScannedFile>& files, const QString& folderPath);
    
signals:
    void folderScanned(const QString& folderPath, const QList<ScannedFile>& files);
    void scanError(const QString& folderPath, const QString& error);
    
private:
    static const QStringList MEDIA_EXTENSIONS;
    bool isMediaFile(const QString& filePath);
    QList<ScannedFile> scanFolderSync(const QString& folderPath);
};

#endif // FOLDERSCANNER_H

