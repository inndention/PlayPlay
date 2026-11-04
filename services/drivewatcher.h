#ifndef DRIVEWATCHER_H
#define DRIVEWATCHER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QFileSystemWatcher>
#include <QMap>
#include <QJsonObject>

struct DriveWatchConfig {
    QStringList includePatterns;
    QStringList excludePatterns;
    bool recursive = true;
    bool watchFiles = true;
    bool watchDirs = false;
};

class DriveWatcher : public QObject
{
    Q_OBJECT
    
public:
    explicit DriveWatcher(QObject* parent = nullptr);
    ~DriveWatcher();
    
    // Watch a directory path
    bool watchPath(const QString& path, const DriveWatchConfig& config = DriveWatchConfig());
    
    // Stop watching a path
    void unwatchPath(const QString& path);
    
    // Get all watched paths
    QStringList getWatchedPaths() const;
    
    // Check if a path is being watched
    bool isWatching(const QString& path) const;
    
signals:
    void fileChanged(const QString& filePath, const QString& watchedPath);
    void fileAdded(const QString& filePath, const QString& watchedPath);
    void fileRemoved(const QString& filePath, const QString& watchedPath);
    void directoryChanged(const QString& dirPath, const QString& watchedPath);
    
private slots:
    void onFileChanged(const QString& path);
    void onDirectoryChanged(const QString& path);
    
private:
    QFileSystemWatcher* m_watcher;
    QMap<QString, DriveWatchConfig> m_configs;
    QMap<QString, QString> m_pathToWatchedPath; // Maps actual path to watched root path
    
    bool shouldProcessPath(const QString& filePath, const DriveWatchConfig& config);
    void addDirectoryRecursive(const QString& dirPath, const QString& watchedPath, const DriveWatchConfig& config);
};

#endif // DRIVEWATCHER_H

